/*
 * LevelStar Icon / APH Braille+ Mobile Manager ("Everest" board).
 *
 * A PXA270-based, speech-and-braille handheld built by LevelStar and sold by
 * the American Printing House for the Blind. The stock firmware is an
 * OpenEmbedded/Poky Linux 2.6.31 system whose entire UI is a Python 2.6
 * application stack driving Eloquence text-to-speech.
 *
 * Everything modelled here was recovered by static analysis of the shipped
 * kernel image; see docs/everest-board.md for the derivation of each constant.
 *
 * This code is licensed under the GNU GPL v2.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/pxa.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/audio/pxa2xx_ac97.h"
#include "hw/block/flash.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "sysemu/blockdev.h"
#include "exec/address-spaces.h"

/*
 * ARM machine type. Recovered from the machine_desc record in the stock
 * kernel: nr = 0x47e.
 */
#define EVEREST_BOARD_ID        1150

/*
 * OneNAND lives on PXA nCS0. The kernel registers a platform device named
 * "onenand" with IORESOURCE_MEM windows covering nCS0 (0x00000000) and nCS1
 * (0x04000000), plus IORESOURCE_IRQ 84. The board file also installs a
 * static mapping of 1MiB at physical 0 (virtual 0xff000000) to reach it.
 *
 * PXA270 has no NAND controller, so a NOR-like OneNAND part on the static
 * memory bus is the only thing that fits; the shipped JFFS2 root image has a
 * 128KiB erase block and carries no inline cleanmarkers, which matches
 * OneNAND geometry (2KiB page / 128KiB block, cleanmarkers in OOB) exactly.
 */
#define EVEREST_ONENAND_BASE    0x00000000

/*
 * Linux 2.6.31 arch/arm/mach-pxa maps GPIO interrupts as
 * IRQ_GPIO(x) = PXA_GPIO_IRQ_BASE(64) + x, so the OneNAND's IRQ 84 is GPIO 20.
 */
#define EVEREST_ONENAND_GPIO    20

/*
 * everest_init() derives two board straps from GPLR3 (the GPIO 96..127 level
 * register at 0x40e00100):
 *
 *     board_id  = (GPLR3 >> 7) & 0xf     -> GPIO 103..106
 *     keypad_id = ((GPLR3 >> 11) & 3)    -> GPIO 107..108  (bits 0..1)
 *               | ((GPLR3 & 1) << 3)     -> GPIO 96        (bit 3)
 *
 * Note bit 2 of keypad_id is never populated by the kernel.
 */
#define EVEREST_GPIO_KEYPAD_ID_BIT3   96
#define EVEREST_GPIO_BOARD_ID_BASE   103   /* 103,104,105,106 = bits 0..3 */
#define EVEREST_GPIO_KEYPAD_ID_BASE  107   /* 107,108         = bits 0..1 */

/*
 * SD/MMC card detect. The /proc/everest/sd_card_present handler reports
 * (GPLR0 >> 4) & 1, so the slot's detect line is GPIO 4, active low as usual
 * for a card-detect switch. Left at QEMU's default of 0 the guest believes a
 * card is inserted and the MMC core retries forever, which floods the console
 * with everest_power_mci/everest_power_off_mci. Drive it high for "empty".
 */
#define EVEREST_GPIO_SD_CARD_DETECT    4

#define EVEREST_DEFAULT_RAM_SIZE    (64 * MiB)

/*
 * Samsung KFG5616 family. QEMU derives the array size from the device id as
 * 1 << (24 + ((id >> 4) & 7)), so 0x30 selects 128MiB.
 */
#define EVEREST_ONENAND_DEVICE_ID   0x30

#define TYPE_EVEREST_MACHINE MACHINE_TYPE_NAME("everest")
OBJECT_DECLARE_SIMPLE_TYPE(EverestMachineState, EVEREST_MACHINE)

struct EverestMachineState {
    MachineState parent_obj;

    uint32_t board_id;
    uint32_t keypad_id;
};

static struct arm_boot_info everest_binfo = {
    .loader_start = PXA2XX_SDRAM_BASE,
};

/* Drive a strap value out over a run of consecutive GPIO input lines. */
static void everest_strap(DeviceState *gpio, int first_gpio,
                          uint32_t value, int nbits)
{
    int i;

    for (i = 0; i < nbits; i++) {
        qemu_set_irq(qdev_get_gpio_in(gpio, first_gpio + i),
                     (value >> i) & 1);
    }
}

static void everest_init(MachineState *machine)
{
    EverestMachineState *ems = EVEREST_MACHINE(machine);
    PXA2xxState *mpu;
    DeviceState *onenand;
    DeviceState *ac97;
    DriveInfo *dinfo;

    mpu = pxa270_init(machine->ram_size, machine->cpu_type);

    /*
     * AC97 audio. QEMU's PXA2xx model has no AC97 unit of its own, so the
     * controller and its WM9713 codec come from hw/audio/pxa2xx_ac97.c. The
     * whole user interface of this machine is spoken, so this is not optional
     * decoration: without it the firmware boots mute.
     */
    ac97 = qdev_new(TYPE_PXA2XX_AC97);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ac97), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ac97), 0, PXA2XX_AC97_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ac97), 0,
                       qdev_get_gpio_in(mpu->pic, PXA2XX_PIC_AC97));
    qdev_connect_gpio_out_named(ac97, "tx-dma", 0,
                                qdev_get_gpio_in(mpu->dma,
                                                 PXA2XX_TX_RQ_AC97_PCM));
    qdev_connect_gpio_out_named(ac97, "rx-dma", 0,
                                qdev_get_gpio_in(mpu->dma,
                                                 PXA2XX_RX_RQ_AC97_PCM));

    /* OneNAND boot flash on nCS0. */
    onenand = qdev_new("onenand");
    qdev_prop_set_uint16(onenand, "manufacturer_id", NAND_MFR_SAMSUNG);
    qdev_prop_set_uint16(onenand, "device_id", EVEREST_ONENAND_DEVICE_ID);
    qdev_prop_set_uint16(onenand, "version_id", 0);
    qdev_prop_set_int32(onenand, "shift", 1);

    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(onenand, "drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(onenand), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(onenand), 0, EVEREST_ONENAND_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(onenand), 0,
                       qdev_get_gpio_in(mpu->gpio, EVEREST_ONENAND_GPIO));

    /* Board straps read by everest_init() out of GPLR3. */
    everest_strap(mpu->gpio, EVEREST_GPIO_BOARD_ID_BASE, ems->board_id, 4);
    everest_strap(mpu->gpio, EVEREST_GPIO_KEYPAD_ID_BASE, ems->keypad_id, 2);
    qemu_set_irq(qdev_get_gpio_in(mpu->gpio, EVEREST_GPIO_KEYPAD_ID_BIT3),
                 (ems->keypad_id >> 3) & 1);

    /* No SD card in the slot (active low detect, so drive it high). */
    qemu_set_irq(qdev_get_gpio_in(mpu->gpio, EVEREST_GPIO_SD_CARD_DETECT), 1);

    everest_binfo.ram_size = machine->ram_size;
    everest_binfo.board_id = EVEREST_BOARD_ID;
    arm_load_kernel(mpu->cpu, machine, &everest_binfo);
}

static void everest_get_board_id(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    EverestMachineState *ems = EVEREST_MACHINE(obj);

    visit_type_uint32(v, name, &ems->board_id, errp);
}

static void everest_set_board_id(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    EverestMachineState *ems = EVEREST_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0xf) {
        error_setg(errp, "board-id must be in the range 0..15");
        return;
    }
    ems->board_id = value;
}

static void everest_get_keypad_id(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    EverestMachineState *ems = EVEREST_MACHINE(obj);

    visit_type_uint32(v, name, &ems->keypad_id, errp);
}

static void everest_set_keypad_id(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    EverestMachineState *ems = EVEREST_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    /* Bit 2 has no GPIO backing it, so it can never be observed. */
    if (value > 0xf || (value & 0x4)) {
        error_setg(errp, "keypad-id must be in the range 0..15 with bit 2 clear");
        return;
    }
    ems->keypad_id = value;
}

static void everest_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "LevelStar Icon / APH Braille+ Mobile Manager (PXA270)";
    mc->init = everest_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("pxa270-c5");
    mc->default_ram_size = EVEREST_DEFAULT_RAM_SIZE;
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;

    object_class_property_add(oc, "board-id", "uint32",
                              everest_get_board_id, everest_set_board_id,
                              NULL, NULL);
    object_class_property_set_description(oc, "board-id",
        "Board revision strap read from GPIO 103..106 (0..15)");

    object_class_property_add(oc, "keypad-id", "uint32",
                              everest_get_keypad_id, everest_set_keypad_id,
                              NULL, NULL);
    object_class_property_set_description(oc, "keypad-id",
        "Keypad variant strap read from GPIO 107,108 and 96 (bit 2 unused)");
}

static void everest_machine_instance_init(Object *obj)
{
    EverestMachineState *ems = EVEREST_MACHINE(obj);

    ems->board_id = 2;
    ems->keypad_id = 0;
}

static const TypeInfo everest_machine_types[] = {
    {
        .name           = TYPE_EVEREST_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(EverestMachineState),
        .instance_init  = everest_machine_instance_init,
        .class_init     = everest_machine_class_init,
    },
};

DEFINE_TYPES(everest_machine_types)
