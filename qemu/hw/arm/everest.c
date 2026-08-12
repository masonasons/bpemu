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
#include "sysemu/reset.h"
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

/*
 * Key lock switch, on GPIO 93 (GPLR2 bit 29; its interrupt is IRQ 157, which
 * /proc/interrupts names "key lock").
 *
 * This one gates the whole keypad. The keypad driver keeps a byte of state that
 * its per-key handler checks first and, if non-zero, returns immediately
 * without reporting anything. The probe initialises that byte to 2 -- neither
 * of the two valid values -- and then samples the switch, deriving
 *
 *     locked = (GPLR2 & (1 << 29)) ? 0 : 1
 *
 * so the pin must be *high* for the keypad to work. Left at QEMU's default of
 * low, every keypress is silently dropped: interrupts arrive and the driver
 * reads the matrix correctly, but nothing ever reaches /dev/input.
 */
#define EVEREST_GPIO_KEY_LOCK         93

/*
 * Keypad matrix.
 *
 * The board's keypad driver programs the PXA27x keypad controller with
 * 0x37dff800, which decodes as an auto-scanned 6-row by 8-column matrix with
 * the matrix and matrix-interrupt enables set. It then looks keycodes up in a
 * table of stacked 48-entry u32 arrays, indexed as
 *
 *     keycodes[variant * 48 + row * 8 + col]
 *
 * with the base array at 0xc03f2dac. The variant is selected at runtime by
 * writing '0' or '1' to a sysfs node; array 0 is the boot default and the only
 * one whose keycodes the probe declares to the input core.
 *
 * The running firmware selects **array 1**, which was confirmed empirically:
 * injecting a key at matrix position 3 reports 362 (KEY_PROGRAM), which is
 * array1[3], not array0[3] (BTN_1). Array 1 is also the fuller layout -- it is
 * the only one containing the six braille dots -- so it is what the host keys
 * below are mapped against:
 *
 *         col0        col1  col2      col3     col4   col5        col6
 *   row0  PROG1       DOT3  INFO      PROGRAM  PROG2  HELP        CANCEL
 *   row1  KPDOT       DOT2  BTN_LEFT  UP       SELECT DOWN        BTN_RIGHT
 *   row2  BTN_0       DOT1  BTN_1     BTN_2    BTN_3  OK          MENU
 *   row3  KPASTERISK  DOT4  BTN_4     RECORD   MUTE   VOLUMEDOWN  VOLUMEUP
 *   row4  DOT6        DOT5  BTN_5     BTN_6    BTN_7  BTN_8       BTN_9
 *   row5  -           -     -         -        -      -           -
 *
 * The dot keys carry keycodes in the 0x600 range, which the driver detects by
 * testing bit 0x200 and routes into the kernel's braille chord assembler
 * rather than reporting directly: pressing dot 3 alone emits an apostrophe,
 * which is what dot 3 means in braille.
 *
 * Column 7 and row 5 are unpopulated. Array 0, for reference, is a plain
 * telephone keypad with no braille keys:
 *
 *         col0        col1       col2  col3       col4      col5    col6
 *   row0  KPASTERISK  BTN_7      BTN_4 BTN_1      OK        INFO    PROG1
 *   row1  BTN_0       BTN_8      BTN_5 BTN_2      MENU      SELECT  DOWN
 *   row2  -           -          -     BTN_RIGHT  BTN_LEFT  UP      PROGRAM
 *   row3  KPDOT       BTN_9      BTN_6 BTN_3      CANCEL    HELP    PROG2
 *   row4  VOLUMEUP    VOLUMEDOWN MUTE  RECORD     -         -       -
 *
 * The host-key assignment below is our choice, not the device's -- the
 * hardware has no PC keyboard. Braille dots use the Perkins home-row
 * convention: S/D/F are dots 3/2/1 and J/K/L are dots 4/5/6.
 */
/*
 * Careful: QEMU's struct keymap is { int8_t column; int8_t row; } -- column
 * first -- so these initialisers are written (col, row), the opposite way round
 * from the (row, col) table above.
 */
#define EVEREST_KP_PROG1      { 0, 0 }
#define EVEREST_KP_DOT3       { 1, 0 }
#define EVEREST_KP_INFO       { 2, 0 }
#define EVEREST_KP_PROGRAM    { 3, 0 }
#define EVEREST_KP_PROG2      { 4, 0 }
#define EVEREST_KP_HELP       { 5, 0 }
#define EVEREST_KP_CANCEL     { 6, 0 }
#define EVEREST_KP_HASH       { 0, 1 }
#define EVEREST_KP_DOT2       { 1, 1 }
#define EVEREST_KP_LEFT       { 2, 1 }
#define EVEREST_KP_UP         { 3, 1 }
#define EVEREST_KP_SELECT     { 4, 1 }
#define EVEREST_KP_DOWN       { 5, 1 }
#define EVEREST_KP_RIGHT      { 6, 1 }
#define EVEREST_KP_0          { 0, 2 }
#define EVEREST_KP_DOT1       { 1, 2 }
#define EVEREST_KP_1          { 2, 2 }
#define EVEREST_KP_2          { 3, 2 }
#define EVEREST_KP_3          { 4, 2 }
#define EVEREST_KP_OK         { 5, 2 }
#define EVEREST_KP_MENU       { 6, 2 }
#define EVEREST_KP_ASTERISK   { 0, 3 }
#define EVEREST_KP_DOT4       { 1, 3 }
#define EVEREST_KP_4          { 2, 3 }
#define EVEREST_KP_RECORD     { 3, 3 }
#define EVEREST_KP_MUTE       { 4, 3 }
#define EVEREST_KP_VOLDOWN    { 5, 3 }
#define EVEREST_KP_VOLUP      { 6, 3 }
#define EVEREST_KP_DOT6       { 0, 4 }
#define EVEREST_KP_DOT5       { 1, 4 }
#define EVEREST_KP_5          { 2, 4 }
#define EVEREST_KP_6          { 3, 4 }
#define EVEREST_KP_7          { 4, 4 }
#define EVEREST_KP_8          { 5, 4 }
#define EVEREST_KP_9          { 6, 4 }

/*
 * Host PC set-1 scancode -> matrix position. QEMU's PXA keypad indexes this by
 * make code, with 0xe0-prefixed keys folded to (code | 0x80).
 */
static const struct keymap everest_keymap[0x100] = {
    [0 ... 0xff]  = { -1, -1 },

    /* Number row doubles as the telephone keypad. */
    [0x02]        = EVEREST_KP_1,
    [0x03]        = EVEREST_KP_2,
    [0x04]        = EVEREST_KP_3,
    [0x05]        = EVEREST_KP_4,
    [0x06]        = EVEREST_KP_5,
    [0x07]        = EVEREST_KP_6,
    [0x08]        = EVEREST_KP_7,
    [0x09]        = EVEREST_KP_8,
    [0x0a]        = EVEREST_KP_9,
    [0x0b]        = EVEREST_KP_0,

    /* Numeric keypad, which is the closer physical analogue. */
    [0x4f]        = EVEREST_KP_1,
    [0x50]        = EVEREST_KP_2,
    [0x51]        = EVEREST_KP_3,
    [0x4b]        = EVEREST_KP_4,
    [0x4c]        = EVEREST_KP_5,
    [0x4d]        = EVEREST_KP_6,
    [0x47]        = EVEREST_KP_7,
    [0x48]        = EVEREST_KP_8,
    [0x49]        = EVEREST_KP_9,
    [0x52]        = EVEREST_KP_0,
    [0x37]        = EVEREST_KP_ASTERISK,   /* keypad *      */
    [0x53]        = EVEREST_KP_HASH,       /* keypad . as # */

    /*
     * Braille dots, Perkins style: left hand S/D/F on dots 3/2/1, right hand
     * J/K/L on dots 4/5/6. Chords are assembled by the guest kernel's braille
     * support, so pressing them together types braille.
     */
    [0x1f]        = EVEREST_KP_DOT3,       /* S */
    [0x20]        = EVEREST_KP_DOT2,       /* D */
    [0x21]        = EVEREST_KP_DOT1,       /* F */
    [0x24]        = EVEREST_KP_DOT4,       /* J */
    [0x25]        = EVEREST_KP_DOT5,       /* K */
    [0x26]        = EVEREST_KP_DOT6,       /* L */

    /* Navigation. */
    [0x1c]        = EVEREST_KP_OK,         /* Enter  */
    [0x01]        = EVEREST_KP_CANCEL,     /* Escape */
    [0xc8]        = EVEREST_KP_UP,         /* Up     */
    [0xd0]        = EVEREST_KP_DOWN,       /* Down   */
    [0xcb]        = EVEREST_KP_LEFT,       /* Left   */
    [0xcd]        = EVEREST_KP_RIGHT,      /* Right  */

    /* Function keys. */
    [0x3b]        = EVEREST_KP_HELP,       /* F1 */
    [0x3c]        = EVEREST_KP_MENU,       /* F2 */
    [0x3d]        = EVEREST_KP_INFO,       /* F3 */
    [0x3e]        = EVEREST_KP_SELECT,     /* F4 */
    [0x3f]        = EVEREST_KP_PROG1,      /* F5 */
    [0x40]        = EVEREST_KP_PROG2,      /* F6 */
    [0x41]        = EVEREST_KP_PROGRAM,    /* F7 */

    /* Media. */
    [0xc9]        = EVEREST_KP_VOLUP,      /* Page Up   */
    [0xd1]        = EVEREST_KP_VOLDOWN,    /* Page Down */
    [0x32]        = EVEREST_KP_MUTE,       /* M */
    [0x13]        = EVEREST_KP_RECORD,     /* R */
};

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
    bool key_lock;
};

static struct arm_boot_info everest_binfo = {
    .loader_start = PXA2XX_SDRAM_BASE,
};

/*
 * The straps are hardwired pin levels, so they have to be re-presented on every
 * system reset. QEMU's pxa2xx-gpio happens not to implement reset today, which
 * would let the initial values survive on their own, but relying on that would
 * make a guest-initiated reboot silently read board_id as 0 the day it grows
 * one.
 */
typedef struct EverestStraps {
    DeviceState *gpio;
    uint32_t board_id;
    uint32_t keypad_id;
    bool key_lock;
} EverestStraps;

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

static void everest_straps_reset(void *opaque)
{
    EverestStraps *st = opaque;

    everest_strap(st->gpio, EVEREST_GPIO_BOARD_ID_BASE, st->board_id, 4);
    everest_strap(st->gpio, EVEREST_GPIO_KEYPAD_ID_BASE, st->keypad_id, 2);
    qemu_set_irq(qdev_get_gpio_in(st->gpio, EVEREST_GPIO_KEYPAD_ID_BIT3),
                 (st->keypad_id >> 3) & 1);

    /* No SD card in the slot (active low detect, so drive it high). */
    qemu_set_irq(qdev_get_gpio_in(st->gpio, EVEREST_GPIO_SD_CARD_DETECT), 1);

    /* Key lock switch: high means unlocked, which is what enables the keypad. */
    qemu_set_irq(qdev_get_gpio_in(st->gpio, EVEREST_GPIO_KEY_LOCK),
                 st->key_lock ? 0 : 1);
}

static void everest_init(MachineState *machine)
{
    EverestMachineState *ems = EVEREST_MACHINE(machine);
    PXA2xxState *mpu;
    DeviceState *onenand;
    DeviceState *ac97;
    DriveInfo *dinfo;
    EverestStraps *straps;

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

    /*
     * Keypad. The board's driver talks to the PXA27x keypad controller that
     * pxa270_init() already instantiates, so all that is needed is the matrix
     * map recovered from the driver's keycode table.
     */
    pxa27x_register_keypad(mpu->kp, everest_keymap,
                           ARRAY_SIZE(everest_keymap));

    /* Board straps read by everest_init() out of GPLR3, plus SD card detect. */
    straps = g_new0(EverestStraps, 1);
    straps->gpio = mpu->gpio;
    straps->board_id = ems->board_id;
    straps->keypad_id = ems->keypad_id;
    straps->key_lock = ems->key_lock;
    qemu_register_reset(everest_straps_reset, straps);
    everest_straps_reset(straps);

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

static bool everest_get_key_lock(Object *obj, Error **errp)
{
    return EVEREST_MACHINE(obj)->key_lock;
}

static void everest_set_key_lock(Object *obj, bool value, Error **errp)
{
    EVEREST_MACHINE(obj)->key_lock = value;
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

    object_class_property_add_bool(oc, "key-lock",
                                   everest_get_key_lock, everest_set_key_lock);
    object_class_property_set_description(oc, "key-lock",
        "Engage the key lock switch (GPIO 93), which disables the keypad");
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
