/*
 * Realtek RTL8150 USB 10/100 Ethernet adapter.
 *
 * The Braille+ has no wired networking and its WiFi/Bluetooth live in a
 * Stonestreet One BGW200 combo part on SPI, driven by a proprietary binary
 * module -- emulating that would mean reverse-engineering an undocumented SPI
 * framing for both a WLAN MAC and a Bluetooth HCI. The firmware does, however,
 * ship rtl8150.ko, and its OHCI host controller works, so presenting this chip
 * gives the device real TCP/IP without touching the radios.
 *
 * QEMU's own usb-net is RNDIS/CDC and cannot help here: this kernel has no
 * usbnet, cdc_ether or rndis_host at all.
 *
 * The register map, endpoint layout and framing below all come from Linux's
 * drivers/net/usb/rtl8150.c, which is the only specification that matters --
 * the guest driver is the sole consumer.
 *
 * This code is licensed under the GNU GPL v2.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/eth.h"
#include "qemu/iov.h"

#define RTL8150_VENDOR_NUM      0x0bda
#define RTL8150_PRODUCT_NUM     0x8150

/* Vendor control requests: bRequest 0x05, direction in bmRequestType. */
#define RTL8150_REQ_REGS        0x05

/*
 * Register file. The driver addresses these with absolute addresses in wValue,
 * starting at IDR; everything it touches lives in the 0x120..0x14f window.
 */
#define RTL8150_REG_BASE        0x0120
#define RTL8150_REG_SIZE        0x0030

#define RTL8150_IDR             0x0120  /* 6 bytes of MAC address */
#define RTL8150_MAR             0x0126
#define RTL8150_CR              0x012e
#define RTL8150_TCR             0x012f
#define RTL8150_RCR             0x0130
#define RTL8150_TSR             0x0132
#define RTL8150_RSR             0x0133
#define RTL8150_CON0            0x0135
#define RTL8150_CON1            0x0136
#define RTL8150_MSR             0x0137  /* media status */
#define RTL8150_PHYADD          0x0138
#define RTL8150_PHYDAT          0x0139  /* 16-bit MII data */
#define RTL8150_PHYCNT          0x013b
#define RTL8150_BMCR            0x0140
#define RTL8150_BMSR            0x0142
#define RTL8150_ANAR            0x0144
#define RTL8150_ANLP            0x0146
#define RTL8150_CSCR            0x014c  /* where the driver reads link state */

/*
 * Bits the guest polls for completion. Both are self-clearing on real silicon,
 * and a model that just stores register writes verbatim hangs the driver:
 * rtl8150_reset() spins on CR_SOFT_RESET until it times out and probe fails
 * with -EIO, and read_mii_word() spins on PHY_GO.
 */
#define CR_SOFT_RESET           0x10
#define PHY_GO                  0x40
#define PHY_WRITE               0x20
#define PHY_REG_MASK            0x1f

/*
 * Media status bits, reported over the interrupt endpoint at INT_MSR. These
 * are low bits, not high ones: intr_callback() drops the carrier whenever
 * MSR_LINK is clear, so getting these wrong turns the link off two lines
 * after set_carrier() turned it on, with nothing in the logs to say so.
 */
#define MSR_DUPLEX              0x10
#define MSR_SPEED               0x08
#define MSR_LINK                0x04

/*
 * rtl8150_open() decides the initial carrier with set_carrier(), which reads
 * CSCR and tests this bit -- not MSR. Leave it clear and the interface comes
 * up BROADCAST MULTICAST without RUNNING, so the stack never transmits and
 * every ping is lost with no error anywhere.
 */
#define CSCR_LINK_STATUS        0x0008

/* Endpoints, per usb_rcvbulkpipe(1) / usb_sndbulkpipe(2) / usb_rcvintpipe(3). */
#define RTL8150_EP_BULK_IN      1
#define RTL8150_EP_BULK_OUT     2
#define RTL8150_EP_INTR_IN      3

/* The driver's rx_urb is RTL8150_MTU long and its status word is 4 bytes. */
#define RTL8150_MTU             1540
#define RTL8150_RX_STATUS_LEN   4

/* INTBUFSIZE, and the offset intr_callback() reads the media status from. */
#define RTL8150_INTR_LEN        8
#define INT_MSR                 2

/* wMaxPacketSize for the bulk endpoints: the unit a transfer is chopped into. */
#define RTL8150_MAX_PACKET      64

#define TYPE_USB_RTL8150 "usb-rtl8150"
OBJECT_DECLARE_SIMPLE_TYPE(RTL8150State, USB_RTL8150)

struct RTL8150State {
    USBDevice dev;

    NICState *nic;
    NICConf conf;

    uint8_t regs[RTL8150_REG_SIZE];

    /*
     * One frame in flight is enough: can_receive() gates the next one. The
     * buffer holds the frame with its status word already appended, and
     * rx_ptr tracks how much of that the host has collected so far, because
     * a transfer larger than wMaxPacketSize is handed over in instalments.
     */
    uint8_t rx_buf[RTL8150_MTU + RTL8150_RX_STATUS_LEN];
    uint32_t rx_total;
    uint32_t rx_ptr;
    bool rx_zlp;

    /* Likewise for transmit: tx_ptr accumulates until a short packet ends it. */
    uint8_t tx_buf[RTL8150_MTU];
    uint32_t tx_ptr;

    USBEndpoint *bulk_in;
    USBEndpoint *intr_in;
    bool debug;             /* BPEMU_RTL8150_DEBUG */
};

enum {
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT,
    STRING_SERIALNUMBER,
};

static const USBDescStrings rtl8150_stringtable = {
    [STRING_MANUFACTURER] = "Realtek",
    [STRING_PRODUCT]      = "RTL8150 USB Ethernet",
    [STRING_SERIALNUMBER] = "1",
};

static const USBDescIface desc_iface_rtl8150 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 3,
    /* The real part reports vendor-specific; rtl8150.ko matches on ID only. */
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0xff,
    .bInterfaceProtocol            = 0xff,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | RTL8150_EP_BULK_IN,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = RTL8150_MAX_PACKET,
        },{
            .bEndpointAddress      = USB_DIR_OUT | RTL8150_EP_BULK_OUT,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = RTL8150_MAX_PACKET,
        },{
            .bEndpointAddress      = USB_DIR_IN | RTL8150_EP_INTR_IN,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = RTL8150_INTR_LEN,
            .bInterval             = 8,
        },
    },
};

static const USBDescDevice desc_device_rtl8150 = {
    .bcdUSB                        = 0x0110,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = USB_CFG_ATT_ONE,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_rtl8150,
        },
    },
};

static const USBDesc desc_rtl8150 = {
    .id = {
        .idVendor                  = RTL8150_VENDOR_NUM,
        .idProduct                 = RTL8150_PRODUCT_NUM,
        .bcdDevice                 = 0x0200,
        .iManufacturer             = STRING_MANUFACTURER,
        .iProduct                  = STRING_PRODUCT,
        .iSerialNumber             = STRING_SERIALNUMBER,
    },
    .full = &desc_device_rtl8150,
    .str  = rtl8150_stringtable,
};

/*
 * Just enough of a PHY to satisfy the driver's link queries. It reads BMSR to
 * decide whether the carrier is up.
 */
static uint16_t rtl8150_mii_read(RTL8150State *s, unsigned reg)
{
    switch (reg) {
    case 0:                     /* BMCR: 100Mb, full duplex, autoneg enabled */
        return 0x3100;
    case 1:                     /* BMSR: autoneg complete, link up */
        return 0x782d;
    case 2:                     /* PHY id */
        return 0x0000;
    case 3:
        return 0x8201;
    case 4:                     /* our advertisement */
        return 0x01e1;
    case 5:                     /* link partner: 100Mb full duplex */
        return 0x45e1;
    default:
        return 0;
    }
}

static void rtl8150_reset_regs(RTL8150State *s)
{
    memset(s->regs, 0, sizeof(s->regs));

    /* The driver reads its MAC straight out of IDR at probe time. */
    memcpy(&s->regs[RTL8150_IDR - RTL8150_REG_BASE],
           s->conf.macaddr.a, ETH_ALEN);

    /* Report a live 100Mb full-duplex link so the driver brings the carrier up. */
    s->regs[RTL8150_MSR - RTL8150_REG_BASE] =
        MSR_LINK | MSR_SPEED | MSR_DUPLEX;

    /* Autonegotiation complete, link up. */
    s->regs[RTL8150_BMSR - RTL8150_REG_BASE] = 0x2d;
    s->regs[RTL8150_BMCR - RTL8150_REG_BASE + 1] = 0x30;

    /* And the register the driver actually consults for the carrier. */
    s->regs[RTL8150_CSCR - RTL8150_REG_BASE] = CSCR_LINK_STATUS;
}

static void rtl8150_handle_reset(USBDevice *dev)
{
    RTL8150State *s = USB_RTL8150(dev);

    s->rx_total = 0;
    s->rx_ptr = 0;
    s->rx_zlp = false;
    s->tx_ptr = 0;
    rtl8150_reset_regs(s);
}

static void rtl8150_handle_control(USBDevice *dev, USBPacket *p,
                                   int request, int value, int index,
                                   int length, uint8_t *data)
{
    RTL8150State *s = USB_RTL8150(dev);
    int ret;
    unsigned offset;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case VendorDeviceRequest | RTL8150_REQ_REGS:        /* register read  */
    case VendorDeviceOutRequest | RTL8150_REQ_REGS:     /* register write */
        if (value < RTL8150_REG_BASE ||
            value + length > RTL8150_REG_BASE + RTL8150_REG_SIZE) {
            p->status = USB_RET_STALL;
            return;
        }
        offset = value - RTL8150_REG_BASE;
        if (request & (USB_DIR_IN << 8)) {
            memcpy(data, s->regs + offset, length);
            p->actual_length = length;
            if (s->debug) {
                fprintf(stderr, "[rtl8150] read  0x%04x len %d ->", value,
                        length);
                for (int i = 0; i < length; i++) {
                    fprintf(stderr, " %02x", data[i]);
                }
                fprintf(stderr, "\n");
            }
        } else {
            if (s->debug) {
                fprintf(stderr, "[rtl8150] write 0x%04x len %d <-", value,
                        length);
                for (int i = 0; i < length; i++) {
                    fprintf(stderr, " %02x", data[i]);
                }
                fprintf(stderr, "\n");
            }
            memcpy(s->regs + offset, data, length);

            /* The soft reset completes at once. */
            if (value <= RTL8150_CR && value + length > RTL8150_CR) {
                s->regs[RTL8150_CR - RTL8150_REG_BASE] &= ~CR_SOFT_RESET;
            }

            /* So does an MII access: publish the result and clear PHY_GO. */
            if (value <= RTL8150_PHYCNT && value + length > RTL8150_PHYCNT) {
                uint8_t *cnt = &s->regs[RTL8150_PHYCNT - RTL8150_REG_BASE];
                uint8_t *dat = &s->regs[RTL8150_PHYDAT - RTL8150_REG_BASE];

                if (*cnt & PHY_GO) {
                    if (!(*cnt & PHY_WRITE)) {
                        uint16_t v = rtl8150_mii_read(s, *cnt & PHY_REG_MASK);
                        dat[0] = v & 0xff;
                        dat[1] = v >> 8;
                    }
                    *cnt &= ~PHY_GO;
                }
            }

            /*
             * Keep the link permanently up: the driver clears MSR while
             * bringing the interface up and then reads it back to decide
             * whether to call netif_carrier_on().
             */
            s->regs[RTL8150_MSR - RTL8150_REG_BASE] =
                MSR_LINK | MSR_SPEED | MSR_DUPLEX;
            s->regs[RTL8150_CSCR - RTL8150_REG_BASE] |= CSCR_LINK_STATUS;
        }
        break;

    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void rtl8150_handle_data(USBDevice *dev, USBPacket *p)
{
    RTL8150State *s = USB_RTL8150(dev);
    uint8_t intr[RTL8150_INTR_LEN];
    uint32_t len;

    switch (p->pid) {
    case USB_TOKEN_OUT:
        if (p->ep->nr != RTL8150_EP_BULK_OUT) {
            goto fail;
        }
        len = sizeof(s->tx_buf) - s->tx_ptr;
        if (len > p->iov.size) {
            len = p->iov.size;
        }
        usb_packet_copy(p, s->tx_buf + s->tx_ptr, len);
        s->tx_ptr += len;
        /*
         * A bulk transfer reaches us as a run of wMaxPacketSize packets that a
         * short one terminates, so a frame only becomes whole once that short
         * packet lands. Sending on every packet instead chops every frame over
         * 64 bytes into pieces: an ARP request fits and gets a reply, while a
         * ping goes out as 64 bytes plus a stray 34-byte fragment and is
         * dropped by the far side, which looks exactly like a receive fault.
         *
         * rtl8150_start_xmit() pads runts to 60 and appends one byte when the
         * length would otherwise be a multiple of 64, which is what guarantees
         * the terminating packet is always short. The padding is harmless
         * trailing bytes to a receiver, so pass the frame on as-is.
         */
        if (p->iov.size % RTL8150_MAX_PACKET || p->iov.size == 0) {
            qemu_send_packet(qemu_get_queue(s->nic), s->tx_buf, s->tx_ptr);
            s->tx_ptr = 0;
        }
        break;

    case USB_TOKEN_IN:
        if (p->ep->nr == RTL8150_EP_BULK_IN) {
            if (!s->rx_total && !s->rx_zlp) {
                p->status = USB_RET_NAK;
                return;
            }
            /*
             * Hand over at most one packet's worth at a time. Copying the
             * whole frame regardless would trip the assert in
             * usb_packet_copy() as soon as anything larger than
             * wMaxPacketSize arrived -- which is every frame of interest,
             * since a 60-byte ARP reply plus its status word is exactly 64.
             */
            len = s->rx_total - s->rx_ptr;
            if (len > p->iov.size) {
                len = p->iov.size;
            }
            usb_packet_copy(p, s->rx_buf + s->rx_ptr, len);
            s->rx_ptr += len;
            if (s->rx_ptr >= s->rx_total) {
                /*
                 * The host reads until a short packet arrives, so a transfer
                 * that happens to be a whole number of packets needs an empty
                 * one to end it, exactly as the silicon would send.
                 */
                s->rx_zlp = s->rx_total != 0 &&
                            s->rx_total % RTL8150_MAX_PACKET == 0;
                s->rx_total = 0;
                s->rx_ptr = 0;
                if (!s->rx_zlp) {
                    qemu_flush_queued_packets(qemu_get_queue(s->nic));
                }
            }
        } else if (p->ep->nr == RTL8150_EP_INTR_IN) {
            memset(intr, 0, sizeof(intr));
            intr[INT_MSR] = s->regs[RTL8150_MSR - RTL8150_REG_BASE];
            usb_packet_copy(p, intr, sizeof(intr));
        } else {
            goto fail;
        }
        break;

    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static bool rtl8150_can_receive(NetClientState *nc)
{
    RTL8150State *s = qemu_get_nic_opaque(nc);

    return !s->rx_total && !s->rx_zlp && s->dev.config;
}

static ssize_t rtl8150_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    RTL8150State *s = qemu_get_nic_opaque(nc);

    if (s->rx_total || s->rx_zlp) {
        return 0;
    }
    if (size > RTL8150_MTU) {
        return -1;
    }

    /*
     * A received transfer is the frame with a four-byte status word appended;
     * the driver takes the length as actual_length - 4. Build it once here so
     * the endpoint handler only has to meter it out.
     */
    memcpy(s->rx_buf, buf, size);
    memset(s->rx_buf + size, 0, RTL8150_RX_STATUS_LEN);
    s->rx_buf[size] = size & 0xff;
    s->rx_buf[size + 1] = (size >> 8) & 0x0f;
    s->rx_total = size + RTL8150_RX_STATUS_LEN;
    s->rx_ptr = 0;
    usb_wakeup(s->bulk_in, 0);
    return size;
}

static void rtl8150_cleanup(NetClientState *nc)
{
    RTL8150State *s = qemu_get_nic_opaque(nc);

    s->nic = NULL;
}

static NetClientInfo net_rtl8150_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = rtl8150_can_receive,
    .receive = rtl8150_receive,
    .cleanup = rtl8150_cleanup,
};

static void rtl8150_realize(USBDevice *dev, Error **errp)
{
    RTL8150State *s = USB_RTL8150(dev);

    usb_desc_create_serial(dev);
    usb_desc_init(dev);

    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, RTL8150_EP_BULK_IN);
    s->intr_in = usb_ep_get(dev, USB_TOKEN_IN, RTL8150_EP_INTR_IN);

    s->debug = getenv("BPEMU_RTL8150_DEBUG") != NULL;
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_rtl8150_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->qdev.id,
                          &dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    rtl8150_reset_regs(s);
}

static void rtl8150_unrealize(USBDevice *dev)
{
    RTL8150State *s = USB_RTL8150(dev);

    qemu_del_nic(s->nic);
}

static const VMStateDescription vmstate_rtl8150 = {
    .name = TYPE_USB_RTL8150,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, RTL8150State),
        VMSTATE_UINT8_ARRAY(regs, RTL8150State, RTL8150_REG_SIZE),
        VMSTATE_UINT8_ARRAY(rx_buf, RTL8150State,
                            RTL8150_MTU + RTL8150_RX_STATUS_LEN),
        VMSTATE_UINT32(rx_total, RTL8150State),
        VMSTATE_UINT32(rx_ptr, RTL8150State),
        VMSTATE_BOOL(rx_zlp, RTL8150State),
        VMSTATE_UINT8_ARRAY(tx_buf, RTL8150State, RTL8150_MTU),
        VMSTATE_UINT32(tx_ptr, RTL8150State),
        VMSTATE_END_OF_LIST()
    }
};

static Property rtl8150_properties[] = {
    DEFINE_NIC_PROPERTIES(RTL8150State, conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void rtl8150_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = rtl8150_realize;
    uc->unrealize      = rtl8150_unrealize;
    uc->product_desc   = "RTL8150 USB Ethernet";
    uc->usb_desc       = &desc_rtl8150;
    uc->handle_reset   = rtl8150_handle_reset;
    uc->handle_control = rtl8150_handle_control;
    uc->handle_data    = rtl8150_handle_data;
    dc->vmsd           = &vmstate_rtl8150;
    device_class_set_props(dc, rtl8150_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo rtl8150_info = {
    .name          = TYPE_USB_RTL8150,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(RTL8150State),
    .class_init    = rtl8150_class_init,
};

static void rtl8150_register_types(void)
{
    type_register_static(&rtl8150_info);
}

type_init(rtl8150_register_types)
