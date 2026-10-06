/**
 * @file ac97.c
 * @brief Intel AC'97 Audio Codec driver implementation
 *
 * Supports DMA-based audio playback via Intel ICH-compatible AC'97 controllers.
 * Uses the Bus Master engine with a Buffer Descriptor List for scatter-gather DMA.
 */

#include "ac97.h"
#include "pci.h"
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "hpet.h"
#include "string.h"
#include <stdarg.h>

/* snprintf is provided by term.c (next to vsnprintf) */
extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* I/O port access */
extern void outb(uint16_t port, uint8_t value);
extern uint8_t inb(uint16_t port);
extern void outw(uint16_t port, uint16_t value);
extern uint16_t inw(uint16_t port);
extern void outl(uint16_t port, uint32_t value);
extern uint32_t inl(uint16_t port);

/** @brief Global AC'97 driver state */
static ac97_state_t ac97 = {0};

/* ============================================================================
 * Low-level codec register access (via NAM I/O port)
 * ============================================================================ */

static uint16_t ac97_nam_read(uint16_t reg) {
    return inw(ac97.nam_port + reg);
}

static void ac97_nam_write(uint16_t reg, uint16_t val) {
    outw(ac97.nam_port + reg, val);
}

/* ============================================================================
 * Low-level bus master register access (via NABM I/O port)
 * ============================================================================ */

static uint8_t ac97_nabm_read8(uint16_t reg) {
    return inb(ac97.nabm_port + reg);
}

static uint16_t ac97_nabm_read16(uint16_t reg) {
    return inw(ac97.nabm_port + reg);
}

static uint32_t ac97_nabm_read32(uint16_t reg) {
    return inl(ac97.nabm_port + reg);
}

static void ac97_nabm_write8(uint16_t reg, uint8_t val) {
    outb(ac97.nabm_port + reg, val);
}

static void ac97_nabm_write16(uint16_t reg, uint16_t val) {
    outw(ac97.nabm_port + reg, val);
}

static void ac97_nabm_write32(uint16_t reg, uint32_t val) {
    outl(ac97.nabm_port + reg, val);
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Find AC'97 PCI device
 *
 * AC'97 audio controllers are PCI class 0x04 (Multimedia), subclass 0x01 (Audio).
 */
static struct pci_device* ac97_find_pci_device(void) {
    /* Try standard audio class first */
    struct pci_device *dev = pci_find_device(0x04, 0x01);
    if (dev) return dev;
    
    /* Some AC'97 controllers register as class 0x04, subclass 0x03 */
    dev = pci_find_device(0x04, 0x03);
    return dev;
}

int ac97_init(void) {
    serial_puts("ac97: initializing AC'97 audio driver\n");

    struct pci_device *dev = ac97_find_pci_device();
    if (!dev) {
        serial_puts("ac97: no AC'97 audio device found on PCI bus\n");
        printf("AC97: No audio device found\n");
        return -1;
    }

    serial_printf("ac97: found audio device - vendor=0x%x device=0x%x\n",
                  dev->vendor_id, dev->device_id);
    printf("AC97: Found audio device (vendor=0x%x, device=0x%x)\n",
           dev->vendor_id, dev->device_id);

    /* Read BARs */
    uint32_t bar0 = pci_device_read(dev, 0x10, 4);
    uint32_t bar1 = pci_device_read(dev, 0x14, 4);

    /* Check that BARs are I/O space (bit 0 = 1) */
    if (!(bar0 & 1) || !(bar1 & 1)) {
        serial_puts("ac97: BARs are not I/O space - unsupported configuration\n");
        printf("AC97: Unsupported BAR configuration (need I/O ports)\n");
        return -1;
    }

    ac97.nam_port  = (uint16_t)(bar0 & 0xFFFC);  /* Mask off I/O bit and reserved */
    ac97.nabm_port = (uint16_t)(bar1 & 0xFFFC);

    serial_printf("ac97: NAM port=0x%x, NABM port=0x%x\n", ac97.nam_port, ac97.nabm_port);

    /* Enable PCI bus mastering and I/O space access */
    uint32_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= (1 << 0) | (1 << 2);  /* I/O space + Bus Master */
    pci_device_write(dev, 0x04, 2, cmd);

    /* Perform cold reset via Global Control */
    ac97_nabm_write32(AC97_NABM_GLB_CTRL, AC97_GC_COLD_RESET);
    hpet_sleep_counter(100);  /* Wait 100ms for codec to settle */

    /* Enable interrupts and set 2-channel mode */
    ac97_nabm_write32(AC97_NABM_GLB_CTRL, AC97_GC_GIE | AC97_GC_2CH_MODE);

    /* Wait for codec ready */
    uint32_t timeout = 1000;
    while (timeout-- > 0) {
        uint32_t status = ac97_nabm_read32(AC97_NABM_GLB_STS);
        if (status & AC97_GS_AD3) {
            serial_puts("ac97: codec is ready\n");
            break;
        }
        hpet_sleep_counter(1);
    }
    if (timeout == 0) {
        serial_puts("ac97: codec ready timeout\n");
        printf("AC97: Codec ready timeout\n");
        return -1;
    }

    /* Reset the codec */
    ac97_nam_write(AC97_NAM_RESET, 0x0000);
    hpet_sleep_counter(10);

    /* Set master volume to reasonable level (0 = max, 0x8000 = mute) */
    ac97_nam_write(AC97_NAM_MASTER_VOL, 0x0303);   /* Both channels -4.5dB */
    ac97_nam_write(AC97_NAM_PCM_OUT_VOL, 0x0808);   /* PCM out, moderate */
    ac97.volume = 75;

    /* Check and enable Variable Rate Audio (VRA) */
    uint16_t ext_id = ac97_nam_read(AC97_NAM_EXTENDED_ID);
    serial_printf("ac97: extended audio ID = 0x%x\n", ext_id);
    
    if (ext_id & 0x0001) {
        /* VRA supported - enable it */
        uint16_t ext_ctrl = ac97_nam_read(AC97_NAM_EXTENDED_CTRL);
        ext_ctrl |= 0x0001;  /* Enable VRA */
        ac97_nam_write(AC97_NAM_EXTENDED_CTRL, ext_ctrl);
        serial_puts("ac97: Variable Rate Audio enabled\n");
    }

    /* Set default sample rate */
    ac97.sample_rate = AC97_DEFAULT_SAMPLE_RATE;
    ac97_nam_write(AC97_NAM_FRONT_DAC_RATE, (uint16_t)ac97.sample_rate);
    uint16_t actual_rate = ac97_nam_read(AC97_NAM_FRONT_DAC_RATE);
    ac97.sample_rate = actual_rate;
    serial_printf("ac97: sample rate set to %d Hz\n", actual_rate);

    /* Allocate Buffer Descriptor List (must be aligned to 8 bytes) */
    ac97.bdl = (ac97_buffer_desc_t *)kalloc_high(
        AC97_BDL_MAX_ENTRIES * sizeof(ac97_buffer_desc_t));
    if (!ac97.bdl) {
        serial_puts("ac97: failed to allocate BDL\n");
        return -1;
    }
    memset(ac97.bdl, 0, AC97_BDL_MAX_ENTRIES * sizeof(ac97_buffer_desc_t));
    ac97.bdl_phys = virt_to_phys(ac97.bdl);

    /* Allocate audio DMA buffers (must be in low memory for 32-bit DMA) */
    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        ac97.buffers[i] = (uint8_t *)kalloc_high(AC97_BUFFER_SIZE);
        if (!ac97.buffers[i]) {
            serial_puts("ac97: failed to allocate audio buffer\n");
            return -1;
        }
        memset(ac97.buffers[i], 0, AC97_BUFFER_SIZE);
        ac97.buffer_phys[i] = virt_to_phys(ac97.buffers[i]);
    }

    /* Reset PCM Out channel */
    ac97_nabm_write8(AC97_NABM_PO_CR, AC97_CR_RR);
    hpet_sleep_counter(10);
    ac97_nabm_write8(AC97_NABM_PO_CR, 0);

    /* Clear status bits */
    ac97_nabm_write16(AC97_NABM_PO_SR,
                      AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);

    ac97.initialized = true;
    ac97.playing = false;
    ac97.current_buffer = 0;

    /* Read codec vendor info */
    uint16_t vid1 = ac97_nam_read(AC97_NAM_VENDOR_ID1);
    uint16_t vid2 = ac97_nam_read(AC97_NAM_VENDOR_ID2);
    serial_printf("ac97: codec vendor ID: 0x%04x%04x\n", vid1, vid2);

    printf("AC97: Audio driver initialized (rate=%dHz)\n", ac97.sample_rate);
    return 0;
}

bool ac97_is_available(void) {
    return ac97.initialized;
}

/* ============================================================================
 * Volume and sample rate control
 * ============================================================================ */

void ac97_set_volume(uint8_t volume) {
    if (!ac97.initialized) return;

    ac97.volume = volume > 100 ? 100 : volume;

    /* AC'97 volume: 0x00 = max, 0x3F = -94.5dB, 0x8000 = mute
     * Each step is -1.5dB. Map 0-100 to the register range. */
    if (volume == 0) {
        ac97_nam_write(AC97_NAM_MASTER_VOL, 0x8000);  /* Mute */
        ac97_nam_write(AC97_NAM_PCM_OUT_VOL, 0x8000);
    } else {
        uint8_t attenuation = (uint8_t)((100 - volume) * 63 / 100);
        uint16_t vol_reg = ((uint16_t)attenuation << 8) | attenuation;
        ac97_nam_write(AC97_NAM_MASTER_VOL, vol_reg);
        ac97_nam_write(AC97_NAM_PCM_OUT_VOL, vol_reg);
    }

    serial_printf("ac97: volume set to %d%%\n", volume);
}

uint32_t ac97_set_sample_rate(uint32_t rate) {
    if (!ac97.initialized) return 0;

    ac97_nam_write(AC97_NAM_FRONT_DAC_RATE, (uint16_t)rate);
    hpet_sleep_counter(5);
    uint16_t actual = ac97_nam_read(AC97_NAM_FRONT_DAC_RATE);
    ac97.sample_rate = actual;

    serial_printf("ac97: sample rate set to %d Hz (requested %d)\n", actual, rate);
    return actual;
}

/* ============================================================================
 * Audio playback
 * ============================================================================ */

/**
 * @brief Set up the BDL and start DMA for the audio buffers
 */
static void ac97_start_dma(void) {
    /* Stop any current playback */
    ac97_nabm_write8(AC97_NABM_PO_CR, 0);

    /* Clear status bits */
    ac97_nabm_write16(AC97_NABM_PO_SR,
                      AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);

    /* Set up BDL entries for our buffers */
    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        ac97.bdl[i].buffer_addr = (uint32_t)ac97.buffer_phys[i];
        /* Length is in samples (16-bit stereo = 4 bytes per sample) */
        ac97.bdl[i].length = AC97_BUFFER_SIZE / 2;  /* samples, not bytes */
        ac97.bdl[i].flags = AC97_BD_IOC;            /* Interrupt on completion */
    }

    /* Set BDL base address */
    ac97_nabm_write32(AC97_NABM_PO_BDBAR, (uint32_t)ac97.bdl_phys);

    /* Set Last Valid Index to last buffer */
    ac97_nabm_write8(AC97_NABM_PO_LVI, AC97_NUM_BUFFERS - 1);

    /* Start playback with interrupt on completion */
    ac97_nabm_write8(AC97_NABM_PO_CR,
                     AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_LVBIE);

    ac97.playing = true;
    serial_puts("ac97: DMA playback started\n");
}

/**
 * @brief Fill a buffer with audio data from the play source
 */
static uint32_t ac97_fill_buffer(int buf_idx) {
    if (!ac97.play_data || ac97.play_pos >= ac97.play_size) {
        /* No more data - fill with silence */
        memset(ac97.buffers[buf_idx], 0, AC97_BUFFER_SIZE);
        return 0;
    }

    uint32_t remaining = ac97.play_size - ac97.play_pos;
    uint32_t to_copy = (remaining < AC97_BUFFER_SIZE) ? remaining : AC97_BUFFER_SIZE;

    memcpy(ac97.buffers[buf_idx], ac97.play_data + ac97.play_pos, to_copy);

    /* Zero-fill the rest if we didn't fill the whole buffer */
    if (to_copy < AC97_BUFFER_SIZE) {
        memset(ac97.buffers[buf_idx] + to_copy, 0, AC97_BUFFER_SIZE - to_copy);
    }

    ac97.play_pos += to_copy;
    return to_copy;
}

int ac97_play_pcm(const uint8_t *data, uint32_t size) {
    if (!ac97.initialized) return -1;
    if (!data || size == 0) return -1;

    /* Stop any current playback */
    ac97_stop();

    /* Set up playback state */
    ac97.play_data = data;
    ac97.play_size = size;
    ac97.play_pos = 0;
    ac97.current_buffer = 0;

    /* Fill initial buffers */
    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        ac97_fill_buffer(i);
    }

    /* Start DMA */
    ac97_start_dma();

    serial_printf("ac97: playing %d bytes of PCM data\n", size);
    return 0;
}

int ac97_play_wav(const uint8_t *wav_data, uint32_t wav_size) {
    if (!ac97.initialized) return -1;
    if (!wav_data || wav_size < 44) return -1;

    /* Parse WAV header */
    if (wav_data[0] != 'R' || wav_data[1] != 'I' ||
        wav_data[2] != 'F' || wav_data[3] != 'F') {
        serial_puts("ac97: not a valid WAV file (missing RIFF header)\n");
        return -1;
    }
    if (wav_data[8] != 'W' || wav_data[9] != 'A' ||
        wav_data[10] != 'V' || wav_data[11] != 'E') {
        serial_puts("ac97: not a valid WAV file (missing WAVE tag)\n");
        return -1;
    }

    /* Extract format info */
    uint16_t audio_format = *(uint16_t *)(wav_data + 20);
    uint16_t num_channels = *(uint16_t *)(wav_data + 22);
    uint32_t sample_rate  = *(uint32_t *)(wav_data + 24);
    uint16_t bits_per_sample = *(uint16_t *)(wav_data + 34);

    serial_printf("ac97: WAV format=%d, channels=%d, rate=%d, bits=%d\n",
                  audio_format, num_channels, sample_rate, bits_per_sample);

    if (audio_format != 1) {
        serial_puts("ac97: only PCM WAV format supported\n");
        return -1;
    }

    /* Set the sample rate to match the WAV file */
    ac97_set_sample_rate(sample_rate);

    /* Find the data chunk */
    uint32_t data_offset = 12;
    uint32_t data_size = 0;
    while (data_offset + 8 < wav_size) {
        if (wav_data[data_offset] == 'd' && wav_data[data_offset + 1] == 'a' &&
            wav_data[data_offset + 2] == 't' && wav_data[data_offset + 3] == 'a') {
            data_size = *(uint32_t *)(wav_data + data_offset + 4);
            data_offset += 8;
            break;
        }
        uint32_t chunk_size = *(uint32_t *)(wav_data + data_offset + 4);
        data_offset += 8 + chunk_size;
    }

    if (data_size == 0) {
        serial_puts("ac97: could not find data chunk in WAV\n");
        return -1;
    }

    /* Clamp data_size to file bounds */
    if (data_offset + data_size > wav_size) {
        data_size = wav_size - data_offset;
    }

    printf("AC97: Playing WAV (%dHz, %dch, %dbit, %d bytes)\n",
           sample_rate, num_channels, bits_per_sample, data_size);

    return ac97_play_pcm(wav_data + data_offset, data_size);
}

void ac97_stop(void) {
    if (!ac97.initialized) return;

    /* Stop DMA */
    ac97_nabm_write8(AC97_NABM_PO_CR, 0);
    hpet_sleep_counter(5);

    /* Clear status */
    ac97_nabm_write16(AC97_NABM_PO_SR,
                      AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);

    ac97.playing = false;
    ac97.play_data = NULL;
    ac97.play_size = 0;
    ac97.play_pos = 0;

    serial_puts("ac97: playback stopped\n");
}

void ac97_resume(void) {
    if (!ac97.initialized || !ac97.play_data) return;

    ac97_nabm_write8(AC97_NABM_PO_CR,
                     AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_LVBIE);
    ac97.playing = true;
}

bool ac97_is_playing(void) {
    if (!ac97.initialized) return false;
    return ac97.playing;
}

void ac97_poll(void) {
    if (!ac97.initialized || !ac97.playing) return;

    /* Read status register */
    uint16_t status = ac97_nabm_read16(AC97_NABM_PO_SR);

    if (status & (AC97_SR_BCIS | AC97_SR_LVBCI)) {
        /* A buffer completed - acknowledge the interrupt */
        ac97_nabm_write16(AC97_NABM_PO_SR, status & (AC97_SR_BCIS | AC97_SR_LVBCI));

        /* Get current index and refill the buffer that just finished */
        uint8_t civ = ac97_nabm_read8(AC97_NABM_PO_CIV);

        /* Fill the buffer that isn't currently being played */
        int fill_idx = (civ + 1) % AC97_NUM_BUFFERS;
        uint32_t filled = ac97_fill_buffer(fill_idx);

        if (filled == 0 && ac97.play_pos >= ac97.play_size) {
            /* All data played */
            ac97_stop();
            serial_puts("ac97: playback complete\n");
            printf("AC97: Playback complete\n");
            return;
        }

        /* Update LVI to keep the DMA running in a ring */
        ac97_nabm_write8(AC97_NABM_PO_LVI, (uint8_t)fill_idx);
    }

    if (status & AC97_SR_FIFOE) {
        /* FIFO error - clear it */
        ac97_nabm_write16(AC97_NABM_PO_SR, AC97_SR_FIFOE);
        serial_puts("ac97: FIFO error\n");
    }
}

/* ============================================================================
 * Test tone generation
 * ============================================================================ */

void ac97_test_tone(uint32_t frequency, uint32_t duration_ms) {
    if (!ac97.initialized) {
        printf("AC97: Not initialized\n");
        return;
    }

    /* Generate a sine wave approximation using a simple square/triangle wave
     * 16-bit signed stereo PCM at the current sample rate */
    uint32_t num_samples = (ac97.sample_rate * duration_ms) / 1000;
    uint32_t data_size = num_samples * 4;  /* 4 bytes per sample (stereo 16-bit) */

    uint8_t *tone_data = (uint8_t *)kalloc(data_size);
    if (!tone_data) {
        printf("AC97: Failed to allocate tone buffer\n");
        return;
    }

    /* Generate a simple square wave */
    int16_t *samples = (int16_t *)tone_data;
    uint32_t period = ac97.sample_rate / frequency;
    int16_t amplitude = 8000;  /* Not too loud */

    for (uint32_t i = 0; i < num_samples; i++) {
        int16_t val = ((i % period) < (period / 2)) ? amplitude : -amplitude;
        samples[i * 2]     = val;  /* Left channel */
        samples[i * 2 + 1] = val;  /* Right channel */
    }

    printf("AC97: Playing %dHz tone for %dms\n", frequency, duration_ms);
    ac97_play_pcm(tone_data, data_size);

    /* Wait for playback (polling mode) */
    while (ac97_is_playing()) {
        ac97_poll();
        hpet_sleep_counter(10);
    }

    kfree(tone_data);
}

void ac97_get_status(char *buf, size_t buflen) {
    if (!ac97.initialized) {
        snprintf(buf, buflen, "AC97: Not initialized (no audio device found)");
        return;
    }

    uint16_t status = ac97_nabm_read16(AC97_NABM_PO_SR);
    uint8_t civ = ac97_nabm_read8(AC97_NABM_PO_CIV);

    snprintf(buf, buflen,
             "AC97: rate=%dHz vol=%d%% playing=%s status=0x%x civ=%d",
             ac97.sample_rate, ac97.volume,
             ac97.playing ? "yes" : "no",
             status, civ);
}
