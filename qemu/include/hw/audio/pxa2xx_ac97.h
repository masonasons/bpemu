/*
 * Intel PXA27x AC97 controller with an on-link Wolfson WM9713 codec.
 *
 * This code is licensed under the GNU GPL v2.
 */

#ifndef HW_AUDIO_PXA2XX_AC97_H
#define HW_AUDIO_PXA2XX_AC97_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_PXA2XX_AC97 "pxa2xx-ac97"
OBJECT_DECLARE_SIMPLE_TYPE(PXA2xxAC97State, PXA2XX_AC97)

/*
 * PXA2xx DMA request lines used by the AC97 unit, as programmed into DRCMR by
 * Linux's sound/soc/pxa/pxa2xx-ac97.c.
 */
#define PXA2XX_RX_RQ_AC97_MIC       8
#define PXA2XX_RX_RQ_AC97_AUX       9
#define PXA2XX_TX_RQ_AC97_AUX       10
#define PXA2XX_RX_RQ_AC97_PCM       11
#define PXA2XX_TX_RQ_AC97_PCM       12

/* PXA27x interrupt number for the AC97 unit. */
#define PXA2XX_PIC_AC97             14

/* Physical base address of the AC97 unit on PXA27x. */
#define PXA2XX_AC97_BASE            0x40500000

#endif /* HW_AUDIO_PXA2XX_AC97_H */
