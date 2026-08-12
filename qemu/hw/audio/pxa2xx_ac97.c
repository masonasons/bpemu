/*
 * Intel PXA27x AC97 controller with an on-link Wolfson WM9713 codec.
 *
 * QEMU's PXA2xx SoC model has never had an AC97 unit (only I2S), and there is
 * no WM9713 model either, so both live here. The Everest board wires its
 * speaker and headphone output through exactly this pair, and the stock
 * firmware's entire user interface is spoken, so without it the machine boots
 * mute and the Eloquence speech engine has nowhere to go.
 *
 * Only the playback path is wired to the host audio backend. Capture is
 * modelled far enough that the guest's ALSA capture streams open and read
 * silence rather than hanging.
 *
 * Register layout follows the PXA27x Developers Manual chapter 13; the reset
 * and codec-access handshakes follow what Linux 2.6.31's
 * sound/arm/pxa2xx-ac97-lib.c actually waits for.
 *
 * This code is licensed under the GNU GPL v2.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "audio/audio.h"
#include "hw/audio/pxa2xx_ac97.h"

/* Controller registers, relative to 0x40500000. */
#define POCR        0x0000  /* PCM out control */
#define PICR        0x0004  /* PCM in control */
#define MCCR        0x0008  /* Mic in control */
#define GCR         0x000c  /* Global control */
#define POSR        0x0010  /* PCM out status */
#define PISR        0x0014  /* PCM in status */
#define MCSR        0x0018  /* Mic in status */
#define GSR         0x001c  /* Global status */
#define CAR         0x0020  /* Codec access */
#define PCDR        0x0040  /* PCM data */
#define MCDR        0x0060  /* Mic-in data */
#define MOCR        0x0100  /* Modem out control */
#define MICR        0x0108  /* Modem in control */
#define MOSR        0x0110  /* Modem out status */
#define MISR        0x0118  /* Modem in status */
#define MODR        0x0140  /* Modem data */

/* Codec register windows. Codec register n appears at window + (n >> 1) * 4. */
#define PAC_BASE    0x0200  /* Primary audio */
#define PMC_BASE    0x0300  /* Primary modem */
#define SAC_BASE    0x0400  /* Secondary audio */
#define SMC_BASE    0x0500  /* Secondary modem */
#define CODEC_WINDOW_SIZE 0x100

/* GCR bits */
#define GCR_CLKBPB      (1 << 31)
#define GCR_nDMAEN      (1 << 24)
#define GCR_CDONE_IE    (1 << 19)
#define GCR_SDONE_IE    (1 << 18)
#define GCR_SECRDY_IEN  (1 << 9)
#define GCR_PRIRDY_IEN  (1 << 8)
#define GCR_SECRES_IEN  (1 << 5)
#define GCR_PRIRES_IEN  (1 << 4)
#define GCR_ACLINK_OFF  (1 << 3)
#define GCR_WARM_RST    (1 << 2)
#define GCR_COLD_RST    (1 << 1)
#define GCR_GIE         (1 << 0)

/* GSR bits (write-1-to-clear) */
#define GSR_CDONE       (1 << 19)
#define GSR_SDONE       (1 << 18)
#define GSR_RDCS        (1 << 15)
#define GSR_SECRES      (1 << 11)
#define GSR_PRIRES      (1 << 10)
#define GSR_SCR         (1 << 9)
#define GSR_PCR         (1 << 8)
#define GSR_MCINT       (1 << 7)
#define GSR_POINT       (1 << 6)
#define GSR_PIINT       (1 << 5)
#define GSR_MOINT       (1 << 2)
#define GSR_MIINT       (1 << 1)
#define GSR_GSCI        (1 << 0)

#define CAR_CAIP        (1 << 0)

/* AC97 codec registers we care about. */
#define AC97_RESET              0x00
#define AC97_POWERDOWN          0x26
#define AC97_EXTENDED_STATUS    0x2a
#define AC97_PCM_FRONT_DAC_RATE 0x2c
/* wm9713_aux_hw_params() programs the Aux DAC rate here. */
#define AC97_PCM_SURR_DAC_RATE  0x2e
#define AC97_PCM_LR_ADC_RATE    0x32
#define AC97_VENDOR_ID1         0x7c
#define AC97_VENDOR_ID2         0x7e

#define WM9713_NUM_REGS 64      /* 0x00..0x7e, 16 bits apiece */

/*
 * Playback FIFO, in stereo frames (one 32-bit PCDR word each: left sample in
 * the low half, right in the high half). Deep enough to ride out host audio
 * scheduling jitter without the guest's DMA ever seeing an underrun.
 */
#define FIFO_FRAMES 4096
/*
 * Assert the DMA request again once at least this many frames are free. The
 * guest bursts 32 bytes at a time, so anything above 8 frames keeps the
 * channel fed while still letting us throttle mid-descriptor.
 */
#define FIFO_REFILL_THRESHOLD 64

/*
 * On real hardware the AC97 unit holds its DMA request line at a level and the
 * DMA engine samples it. QEMU's pxa2xx-dma instead latches request *edges*, and
 * drops any edge that arrives before the guest has set DRCMR_MAPVLD for the
 * channel -- which is exactly when we first raise it, during codec bring-up.
 * Without a nudge afterwards the channel never starts and the guest's writes
 * fail with -EIO. Re-presenting the level on a slow timer restores the
 * level-sensitive behaviour; once samples are flowing the PCDR writes and the
 * audio callback keep the line up to date and this only idles.
 */
#define DMA_REQUEST_REFRESH_NS (2 * SCALE_MS)

struct PXA2xxAC97State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq tx_dma;    /* DRCMR(12): AC97 PCM out */
    qemu_irq rx_dma;    /* DRCMR(11): AC97 PCM in  */

    uint32_t gcr;
    uint32_t gsr;
    uint32_t pocr, picr, mccr;
    uint32_t posr, pisr, mcsr;
    uint32_t mocr, micr, mosr, misr;

    uint16_t codec[WM9713_NUM_REGS];

    QEMUSoundCard card;
    SWVoiceOut *voice;
    uint32_t freq;          /* rate the voice is currently open at */
    bool voice_active;

    uint32_t fifo[FIFO_FRAMES];
    uint32_t fifo_head;     /* next slot to write */
    uint32_t fifo_tail;     /* next slot to read  */
    uint32_t fifo_level;    /* frames currently queued */
    bool drained_last_cb;

    /*
     * The WM9713 has a second, mono DAC -- the "Aux" DAC -- which ALSA exposes
     * as a separate PCM device and Linux feeds through MODR on DMA request 10.
     * The Braille+ speaks through it: Eloquence synthesises mono, and the
     * shell plays it on the Aux device while GStreamer sound effects go to the
     * stereo HiFi DAC. Dropping MODR therefore loses all speech while leaving
     * every other sound working, which is exactly how this presented.
     */
    SWVoiceOut *aux_voice;
    uint32_t aux_freq;
    bool aux_active;
    int16_t aux_fifo[FIFO_FRAMES];
    uint32_t aux_head;
    uint32_t aux_tail;
    uint32_t aux_level;     /* mono samples currently queued */
    bool aux_drained_last_cb;
    qemu_irq aux_tx_dma;    /* DRCMR(10) */
    uint64_t aux_pushed;    /* debug: samples pushed this burst */
    uint64_t pcm_pushed;    /* debug: frames pushed this burst */

    QEMUTimer *dma_refresh;
    bool debug;             /* BPEMU_AC97_DEBUG: report burst sizes */
};

/*
 * WM9713 power-on register defaults. Linux's wm9713 codec driver decides the
 * part is present by resetting it and checking that register 0 reads back as
 * 0x6174, and snd_soc reads the vendor ID from 0x7c/0x7e, so at minimum those
 * three have to be right.
 */
static const uint16_t wm9713_default_regs[WM9713_NUM_REGS] = {
    0x6174, 0x8080, 0x8080, 0x8080,
    0xc880, 0xe808, 0xe808, 0x0808,
    0x00da, 0x8000, 0xd600, 0xaaa0,
    0xaaa0, 0xaaa0, 0x0000, 0x0000,
    0x0f0f, 0x0040, 0x0000, 0x7f00,
    0x0405, 0x0410, 0xbb80, 0xbb80,
    0x0000, 0xbb80, 0x0000, 0x4000,
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x2000, 0x2000, 0x0000,
    0x0000, 0x0000, 0xf83e, 0x0808,
    0xf83e, 0x0000, 0x0000, 0x0000,
    0x8000, 0x8000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x574d, 0x4c13,
};

static void pxa2xx_ac97_update_irq(PXA2xxAC97State *s)
{
    uint32_t mask = 0;

    if (s->gcr & GCR_CDONE_IE) {
        mask |= GSR_CDONE;
    }
    if (s->gcr & GCR_SDONE_IE) {
        mask |= GSR_SDONE;
    }
    if (s->gcr & GCR_PRIRDY_IEN) {
        mask |= GSR_PCR;
    }
    if (s->gcr & GCR_SECRDY_IEN) {
        mask |= GSR_SCR;
    }
    if (s->gcr & GCR_PRIRES_IEN) {
        mask |= GSR_PRIRES;
    }
    if (s->gcr & GCR_SECRES_IEN) {
        mask |= GSR_SECRES;
    }

    qemu_set_irq(s->irq, (s->gcr & GCR_GIE) && (s->gsr & mask));
}

static bool pxa2xx_ac97_link_up(PXA2xxAC97State *s)
{
    return !(s->gcr & GCR_ACLINK_OFF) && !(s->gcr & GCR_nDMAEN) &&
           (s->gcr & (GCR_COLD_RST | GCR_WARM_RST));
}

static void pxa2xx_ac97_update_dma(PXA2xxAC97State *s)
{
    bool link_up = pxa2xx_ac97_link_up(s);
    bool want_tx = link_up &&
                   (FIFO_FRAMES - s->fifo_level) >= FIFO_REFILL_THRESHOLD;

    bool want_aux = link_up &&
                    (FIFO_FRAMES - s->aux_level) >= FIFO_REFILL_THRESHOLD;

    qemu_set_irq(s->tx_dma, want_tx);
    qemu_set_irq(s->aux_tx_dma, want_aux);
    /* Capture is silence-only; never ask the DMA engine for input frames. */
    qemu_set_irq(s->rx_dma, 0);

    if (link_up) {
        timer_mod(s->dma_refresh,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  DMA_REQUEST_REFRESH_NS);
    } else {
        timer_del(s->dma_refresh);
    }
}

static void pxa2xx_ac97_dma_refresh(void *opaque)
{
    pxa2xx_ac97_update_dma(opaque);
}

static uint32_t pxa2xx_ac97_playback_freq(PXA2xxAC97State *s)
{
    uint32_t freq = s->codec[AC97_PCM_FRONT_DAC_RATE >> 1];

    /* A codec that has not been programmed yet reads back as 0. */
    if (freq < 4000 || freq > 48000) {
        freq = 48000;
    }
    return freq;
}

static uint32_t pxa2xx_ac97_aux_freq(PXA2xxAC97State *s)
{
    uint32_t freq = s->codec[AC97_PCM_SURR_DAC_RATE >> 1];

    /*
     * Fall back to the front DAC rate if the Aux rate has not been programmed.
     * Eloquence synthesises at 11025Hz, so this is rarely the same as the HiFi
     * stream's rate and the two need separate voices.
     */
    if (freq < 4000 || freq > 48000) {
        freq = pxa2xx_ac97_playback_freq(s);
    }
    return freq;
}

static void pxa2xx_ac97_out_cb(void *opaque, int free_bytes);
static void pxa2xx_ac97_aux_cb(void *opaque, int free_bytes);

static void pxa2xx_ac97_open_aux_voice(PXA2xxAC97State *s)
{
    struct audsettings as = {
        .freq = pxa2xx_ac97_aux_freq(s),
        .nchannels = 1,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };

    s->aux_freq = as.freq;
    s->aux_voice = AUD_open_out(&s->card, s->aux_voice, "pxa2xx-ac97.aux",
                                s, pxa2xx_ac97_aux_cb, &as);
    if (s->debug) {
        fprintf(stderr, "[ac97] aux voice opened at %u Hz (surr=0x%04x "
                "front=0x%04x extstat=0x%04x)\n", as.freq,
                s->codec[AC97_PCM_SURR_DAC_RATE >> 1],
                s->codec[AC97_PCM_FRONT_DAC_RATE >> 1],
                s->codec[AC97_EXTENDED_STATUS >> 1]);
    }
}

static void pxa2xx_ac97_open_voice(PXA2xxAC97State *s)
{
    struct audsettings as = {
        .freq = pxa2xx_ac97_playback_freq(s),
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };

    s->freq = as.freq;
    s->voice = AUD_open_out(&s->card, s->voice, "pxa2xx-ac97.out",
                            s, pxa2xx_ac97_out_cb, &as);
}

/* Push queued frames at the backend. Returns the number of frames consumed. */
static uint32_t pxa2xx_ac97_drain(PXA2xxAC97State *s, uint32_t max_bytes)
{
    uint32_t consumed = 0;

    while (s->fifo_level && max_bytes >= sizeof(uint32_t)) {
        /* Write the run up to the end of the ring in one go. */
        uint32_t contig = MIN(s->fifo_level, FIFO_FRAMES - s->fifo_tail);
        uint32_t want = MIN(contig * sizeof(uint32_t), max_bytes);
        size_t written = AUD_write(s->voice, &s->fifo[s->fifo_tail], want);
        uint32_t frames = written / sizeof(uint32_t);

        if (!frames) {
            break;
        }
        s->fifo_tail = (s->fifo_tail + frames) % FIFO_FRAMES;
        s->fifo_level -= frames;
        max_bytes -= frames * sizeof(uint32_t);
        consumed += frames;
    }

    return consumed;
}

static void pxa2xx_ac97_out_cb(void *opaque, int free_bytes)
{
    PXA2xxAC97State *s = opaque;

    if (!s->voice) {
        return;
    }

    pxa2xx_ac97_drain(s, free_bytes);

    /*
     * Idle the voice once we have gone two callbacks with nothing queued,
     * so a silent guest does not hold the host output device open.
     */
    if (!s->fifo_level) {
        if (s->drained_last_cb && s->voice_active) {
            AUD_set_active_out(s->voice, 0);
            s->voice_active = false;
            if (s->debug) {
                fprintf(stderr, "[ac97] PCM burst: %llu frames at %u Hz = "
                        "%.2f s\n", (unsigned long long)s->pcm_pushed,
                        s->freq, (double)s->pcm_pushed / s->freq);
            }
            s->pcm_pushed = 0;
        }
        s->drained_last_cb = true;
    } else {
        s->drained_last_cb = false;
    }

    pxa2xx_ac97_update_dma(s);
}

static void pxa2xx_ac97_aux_cb(void *opaque, int free_bytes)
{
    PXA2xxAC97State *s = opaque;
    uint32_t max_bytes = free_bytes;

    if (!s->aux_voice) {
        return;
    }

    while (s->aux_level && max_bytes >= sizeof(int16_t)) {
        uint32_t contig = MIN(s->aux_level, FIFO_FRAMES - s->aux_tail);
        uint32_t want = MIN(contig * sizeof(int16_t), max_bytes);
        size_t written = AUD_write(s->aux_voice, &s->aux_fifo[s->aux_tail],
                                   want);
        uint32_t samples = written / sizeof(int16_t);

        if (!samples) {
            break;
        }
        s->aux_tail = (s->aux_tail + samples) % FIFO_FRAMES;
        s->aux_level -= samples;
        max_bytes -= samples * sizeof(int16_t);
    }

    if (!s->aux_level) {
        if (s->aux_drained_last_cb && s->aux_active) {
            AUD_set_active_out(s->aux_voice, 0);
            s->aux_active = false;
            if (s->debug) {
                fprintf(stderr, "[ac97] AUX burst: %llu samples at %u Hz = "
                        "%.2f s\n", (unsigned long long)s->aux_pushed,
                        s->aux_freq, (double)s->aux_pushed / s->aux_freq);
            }
            s->aux_pushed = 0;
        }
        s->aux_drained_last_cb = true;
    } else {
        s->aux_drained_last_cb = false;
    }

    pxa2xx_ac97_update_dma(s);
}

static void pxa2xx_ac97_push_aux_sample(PXA2xxAC97State *s, int16_t sample)
{
    if (s->aux_level == FIFO_FRAMES) {
        s->aux_tail = (s->aux_tail + 1) % FIFO_FRAMES;
        s->aux_level--;
        s->mosr |= 1 << 4;
    }

    s->aux_fifo[s->aux_head] = sample;
    s->aux_head = (s->aux_head + 1) % FIFO_FRAMES;
    s->aux_level++;
    s->aux_drained_last_cb = false;

    s->aux_pushed++;

    if (s->aux_voice && !s->aux_active) {
        AUD_set_active_out(s->aux_voice, 1);
        s->aux_active = true;
    }
}

static void pxa2xx_ac97_push_frame(PXA2xxAC97State *s, uint32_t frame)
{
    if (s->fifo_level == FIFO_FRAMES) {
        /* Overrun: drop the oldest frame so playback keeps moving. */
        s->fifo_tail = (s->fifo_tail + 1) % FIFO_FRAMES;
        s->fifo_level--;
        s->posr |= 1 << 4;      /* FU: FIFO underrun/overrun */
    }

    s->fifo[s->fifo_head] = frame;
    s->fifo_head = (s->fifo_head + 1) % FIFO_FRAMES;
    s->fifo_level++;
    s->drained_last_cb = false;
    s->pcm_pushed++;

    if (s->voice && !s->voice_active) {
        AUD_set_active_out(s->voice, 1);
        s->voice_active = true;
    }
}

/* Codec register access. Returns true if the address was inside a window. */
static bool pxa2xx_ac97_codec_addr(hwaddr addr, unsigned *window, unsigned *reg)
{
    if (addr >= PAC_BASE && addr < PAC_BASE + CODEC_WINDOW_SIZE) {
        *window = PAC_BASE;
    } else if (addr >= PMC_BASE && addr < PMC_BASE + CODEC_WINDOW_SIZE) {
        *window = PMC_BASE;
    } else if (addr >= SAC_BASE && addr < SAC_BASE + CODEC_WINDOW_SIZE) {
        *window = SAC_BASE;
    } else if (addr >= SMC_BASE && addr < SMC_BASE + CODEC_WINDOW_SIZE) {
        *window = SMC_BASE;
    } else {
        return false;
    }

    *reg = ((addr - *window) >> 2) & (WM9713_NUM_REGS - 1);
    return true;
}

static void pxa2xx_ac97_codec_write(PXA2xxAC97State *s, unsigned reg,
                                    uint16_t value)
{
    switch (reg << 1) {
    case AC97_RESET:
        /* Any write to the reset register restores codec defaults. */
        memcpy(s->codec, wm9713_default_regs, sizeof(s->codec));
        return;
    case AC97_VENDOR_ID1:
    case AC97_VENDOR_ID2:
        return;             /* read-only */
    }

    s->codec[reg] = value;

    if ((reg << 1) == AC97_PCM_FRONT_DAC_RATE &&
        pxa2xx_ac97_playback_freq(s) != s->freq) {
        pxa2xx_ac97_open_voice(s);
        if (s->voice_active) {
            AUD_set_active_out(s->voice, 1);
        }
    }

    if ((reg << 1) == AC97_PCM_SURR_DAC_RATE &&
        pxa2xx_ac97_aux_freq(s) != s->aux_freq) {
        pxa2xx_ac97_open_aux_voice(s);
        if (s->aux_active) {
            AUD_set_active_out(s->aux_voice, 1);
        }
    }
}

static uint64_t pxa2xx_ac97_read(void *opaque, hwaddr addr, unsigned size)
{
    PXA2xxAC97State *s = opaque;
    unsigned window, reg;

    if (pxa2xx_ac97_codec_addr(addr, &window, &reg)) {
        if (window != PAC_BASE) {
            /*
             * Only a primary audio codec is fitted. Leaving GSR_SDONE clear
             * is how the driver discovers that nothing answered.
             */
            return 0;
        }
        s->gsr |= GSR_SDONE;
        pxa2xx_ac97_update_irq(s);
        return s->codec[reg];
    }

    switch (addr) {
    case GCR:
        return s->gcr;
    case GSR:
        return s->gsr;
    case CAR:
        return 0;           /* never busy: accesses complete immediately */
    case POCR:
        return s->pocr;
    case PICR:
        return s->picr;
    case MCCR:
        return s->mccr;
    case POSR:
        return s->posr;
    case PISR:
        return s->pisr;
    case MCSR:
        return s->mcsr;
    case MOCR:
        return s->mocr;
    case MICR:
        return s->micr;
    case MOSR:
        return s->mosr;
    case MISR:
        return s->misr;
    case PCDR:
    case MCDR:
    case MODR:
        return 0;           /* capture reads back silence */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void pxa2xx_ac97_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    PXA2xxAC97State *s = opaque;
    unsigned window, reg;

    if (pxa2xx_ac97_codec_addr(addr, &window, &reg)) {
        if (window == PAC_BASE) {
            pxa2xx_ac97_codec_write(s, reg, value);
            s->gsr |= GSR_CDONE;
            pxa2xx_ac97_update_irq(s);
        }
        return;
    }

    switch (addr) {
    case GCR: {
        uint32_t old = s->gcr;

        s->gcr = value;
        /*
         * Linux drives a cold reset by clearing GCR entirely and then writing
         * GCR_COLD_RST | GCR_WARM_RST; a warm reset just ORs in GCR_WARM_RST.
         * Either way, taking a reset line high is what makes the codec report
         * ready, which is the edge pxa2xx-ac97-lib.c waits on.
         */
        if ((value & (GCR_COLD_RST | GCR_WARM_RST)) &&
            !(value & GCR_ACLINK_OFF)) {
            if (value & GCR_COLD_RST && !(old & GCR_COLD_RST)) {
                memcpy(s->codec, wm9713_default_regs, sizeof(s->codec));
            }
            s->gsr |= GSR_PCR;
        }
        pxa2xx_ac97_update_irq(s);
        pxa2xx_ac97_update_dma(s);
        break;
    }
    case GSR:
        s->gsr &= ~(uint32_t)value;     /* write 1 to clear */
        pxa2xx_ac97_update_irq(s);
        break;
    case CAR:
        break;
    case POCR:
        s->pocr = value;
        pxa2xx_ac97_update_dma(s);
        break;
    case PICR:
        s->picr = value;
        break;
    case MCCR:
        s->mccr = value;
        break;
    case POSR:
        s->posr &= ~(uint32_t)value;
        break;
    case PISR:
        s->pisr &= ~(uint32_t)value;
        break;
    case MCSR:
        s->mcsr &= ~(uint32_t)value;
        break;
    case MOCR:
        s->mocr = value;
        break;
    case MICR:
        s->micr = value;
        break;
    case MOSR:
        s->mosr &= ~(uint32_t)value;
        break;
    case MISR:
        s->misr &= ~(uint32_t)value;
        break;
    case PCDR:
        pxa2xx_ac97_push_frame(s, value);
        pxa2xx_ac97_update_dma(s);
        break;
    case MODR:
        /*
         * One mono sample per write, in the low half; the upper half is
         * padding. It is tempting to read a 32-bit write as two packed S16
         * samples, since the DMA is configured DCMD_WIDTH4 over what looks
         * like a linear mono buffer -- but that doubles the stream. Measured:
         * a 3.03s clip arrives as ~147k writes, matching one sample each.
         */
        pxa2xx_ac97_push_aux_sample(s, (int16_t)(value & 0xffff));
        pxa2xx_ac97_update_dma(s);
        break;
    case MCDR:
        break;              /* microphone input is not wired anywhere */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps pxa2xx_ac97_ops = {
    .read = pxa2xx_ac97_read,
    .write = pxa2xx_ac97_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void pxa2xx_ac97_reset_hold(Object *obj, ResetType type)
{
    PXA2xxAC97State *s = PXA2XX_AC97(obj);

    s->gcr = 0;
    s->gsr = 0;
    s->pocr = s->picr = s->mccr = 0;
    s->posr = s->pisr = s->mcsr = 0;
    s->mocr = s->micr = s->mosr = s->misr = 0;
    s->fifo_head = s->fifo_tail = s->fifo_level = 0;
    s->drained_last_cb = true;
    s->aux_head = s->aux_tail = s->aux_level = 0;
    s->aux_drained_last_cb = true;
    memcpy(s->codec, wm9713_default_regs, sizeof(s->codec));

    if (s->voice && s->voice_active) {
        AUD_set_active_out(s->voice, 0);
        s->voice_active = false;
    }
    if (s->aux_voice && s->aux_active) {
        AUD_set_active_out(s->aux_voice, 0);
        s->aux_active = false;
    }
    if (s->dma_refresh) {
        timer_del(s->dma_refresh);
    }

    qemu_set_irq(s->irq, 0);
    qemu_set_irq(s->tx_dma, 0);
    qemu_set_irq(s->aux_tx_dma, 0);
    qemu_set_irq(s->rx_dma, 0);
}

static void pxa2xx_ac97_realize(DeviceState *dev, Error **errp)
{
    PXA2xxAC97State *s = PXA2XX_AC97(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!AUD_register_card("pxa2xx-ac97", &s->card, errp)) {
        return;
    }

    s->debug = getenv("BPEMU_AC97_DEBUG") != NULL;
    s->dma_refresh = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  pxa2xx_ac97_dma_refresh, s);

    memcpy(s->codec, wm9713_default_regs, sizeof(s->codec));
    pxa2xx_ac97_open_voice(s);
    if (!s->voice) {
        error_setg(errp, "could not open the host audio output");
        return;
    }
    AUD_set_volume_out(s->voice, 0, 255, 255);

    pxa2xx_ac97_open_aux_voice(s);
    if (!s->aux_voice) {
        error_setg(errp, "could not open the host audio output for the Aux DAC");
        return;
    }
    AUD_set_volume_out(s->aux_voice, 0, 255, 255);

    memory_region_init_io(&s->iomem, OBJECT(s), &pxa2xx_ac97_ops, s,
                          "pxa2xx-ac97", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(dev, &s->tx_dma, "tx-dma", 1);
    qdev_init_gpio_out_named(dev, &s->aux_tx_dma, "aux-tx-dma", 1);
    qdev_init_gpio_out_named(dev, &s->rx_dma, "rx-dma", 1);
}

static void pxa2xx_ac97_unrealize(DeviceState *dev)
{
    PXA2xxAC97State *s = PXA2XX_AC97(dev);

    if (s->dma_refresh) {
        timer_free(s->dma_refresh);
        s->dma_refresh = NULL;
    }
    AUD_close_out(&s->card, s->voice);
    AUD_close_out(&s->card, s->aux_voice);
    AUD_remove_card(&s->card);
}

static int pxa2xx_ac97_post_load(void *opaque, int version_id)
{
    PXA2xxAC97State *s = opaque;

    /*
     * Snapshots are the whole point of this machine for anyone who does not
     * want to sit through a TCG boot, so make sure a restored state resumes
     * presenting the DMA request rather than going quiet.
     */
    pxa2xx_ac97_update_dma(s);
    if (s->voice) {
        AUD_set_active_out(s->voice, s->voice_active);
    }
    if (s->aux_voice) {
        AUD_set_active_out(s->aux_voice, s->aux_active);
    }
    return 0;
}

static const VMStateDescription vmstate_pxa2xx_ac97 = {
    .name = "pxa2xx-ac97",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pxa2xx_ac97_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gcr, PXA2xxAC97State),
        VMSTATE_UINT32(gsr, PXA2xxAC97State),
        VMSTATE_UINT32(pocr, PXA2xxAC97State),
        VMSTATE_UINT32(picr, PXA2xxAC97State),
        VMSTATE_UINT32(mccr, PXA2xxAC97State),
        VMSTATE_UINT32(posr, PXA2xxAC97State),
        VMSTATE_UINT32(pisr, PXA2xxAC97State),
        VMSTATE_UINT32(mcsr, PXA2xxAC97State),
        VMSTATE_UINT32(mocr, PXA2xxAC97State),
        VMSTATE_UINT32(micr, PXA2xxAC97State),
        VMSTATE_UINT32(mosr, PXA2xxAC97State),
        VMSTATE_UINT32(misr, PXA2xxAC97State),
        VMSTATE_UINT16_ARRAY(codec, PXA2xxAC97State, WM9713_NUM_REGS),
        VMSTATE_UINT32_ARRAY(fifo, PXA2xxAC97State, FIFO_FRAMES),
        VMSTATE_UINT32(fifo_head, PXA2xxAC97State),
        VMSTATE_UINT32(fifo_tail, PXA2xxAC97State),
        VMSTATE_UINT32(fifo_level, PXA2xxAC97State),
        VMSTATE_BOOL(voice_active, PXA2xxAC97State),
        VMSTATE_INT16_ARRAY(aux_fifo, PXA2xxAC97State, FIFO_FRAMES),
        VMSTATE_UINT32(aux_head, PXA2xxAC97State),
        VMSTATE_UINT32(aux_tail, PXA2xxAC97State),
        VMSTATE_UINT32(aux_level, PXA2xxAC97State),
        VMSTATE_BOOL(aux_active, PXA2xxAC97State),
        VMSTATE_TIMER_PTR(dma_refresh, PXA2xxAC97State),
        VMSTATE_END_OF_LIST()
    }
};

static Property pxa2xx_ac97_properties[] = {
    DEFINE_AUDIO_PROPERTIES(PXA2xxAC97State, card),
    DEFINE_PROP_END_OF_LIST(),
};

static void pxa2xx_ac97_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, pxa2xx_ac97_properties);
    dc->realize = pxa2xx_ac97_realize;
    dc->unrealize = pxa2xx_ac97_unrealize;
    dc->vmsd = &vmstate_pxa2xx_ac97;
    dc->desc = "PXA27x AC97 controller with WM9713 codec";
    rc->phases.hold = pxa2xx_ac97_reset_hold;
}

static const TypeInfo pxa2xx_ac97_info = {
    .name          = TYPE_PXA2XX_AC97,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PXA2xxAC97State),
    .class_init    = pxa2xx_ac97_class_init,
};

static void pxa2xx_ac97_register_types(void)
{
    type_register_static(&pxa2xx_ac97_info);
}

type_init(pxa2xx_ac97_register_types)
