/**
 * @file audio_mixer.c
 * @brief Software audio mixer implementation
 *
 * Combines multiple audio streams into a single stereo PCM output.
 * The mixed output is fed to the AC'97 DMA engine for hardware playback.
 */

#include "audio_mixer.h"
#include "ac97.h"
#include "mem.h"
#include "serial.h"
#include "hpet.h"
#include "string.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);

/** Global mixer state */
static audio_mixer_t mixer = {0};

/* Tone data buffer (allocated once for generated tones) */
static uint8_t *tone_buffer = NULL;
#define TONE_MAX_SIZE (MIXER_SAMPLE_RATE * 4 * 5) /* 5 seconds max at stereo 16-bit */

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/** Find a free channel, or return -1 */
static int mixer_find_free_channel(void) {
    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) {
        if (mixer.channels[i].state == MIXER_CHAN_FREE ||
            mixer.channels[i].state == MIXER_CHAN_DONE) {
            return i;
        }
    }
    return -1;
}

/** Read one sample from a channel at the current position.
 *  Returns a 16-bit signed sample. Handles 8-bit unsigned conversion. */
static inline int16_t channel_read_sample(mixer_channel_t *ch) {
    if (ch->position >= ch->data_size) return 0;

    if (ch->format.bits == 16) {
        /* 16-bit signed little-endian */
        if (ch->position + 1 >= ch->data_size) return 0;
        int16_t sample = (int16_t)(ch->data[ch->position] | (ch->data[ch->position + 1] << 8));
        ch->position += 2;
        return sample;
    } else if (ch->format.bits == 8) {
        /* 8-bit unsigned, convert to 16-bit signed */
        uint8_t sample = ch->data[ch->position];
        ch->position += 1;
        return (int16_t)((sample - 128) << 8);
    }
    return 0;
}

/** Read one stereo pair (left, right) from a channel.
 *  Mono sources are duplicated to both channels. */
static void channel_read_stereo(mixer_channel_t *ch, int16_t *left, int16_t *right) {
    if (ch->format.channels >= 2) {
        *left  = channel_read_sample(ch);
        *right = channel_read_sample(ch);
    } else {
        /* Mono: duplicate to both channels */
        int16_t sample = channel_read_sample(ch);
        *left  = sample;
        *right = sample;
    }
}

/** Clamp a 32-bit value to 16-bit range */
static inline int16_t clamp16(int32_t val) {
    if (val > 32767)  return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int mixer_init(void) {
    serial_puts("mixer: initializing audio mixer\n");

    memset(&mixer, 0, sizeof(mixer));
    mixer.master_volume = 200;  /* ~78% default */

    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) {
        mixer.channels[i].state = MIXER_CHAN_FREE;
        mixer.channels[i].volume = 255;
        mixer.channels[i].pan = 0;
    }

    /* Ensure AC97 is initialized */
    if (!ac97_is_available()) {
        serial_puts("mixer: AC97 not available, mixer will be inactive\n");
        mixer.initialized = true;  /* still track state */
        return 0;
    }

    mixer.initialized = true;
    serial_puts("mixer: initialized with 8 channels\n");
    return 0;
}

bool mixer_is_available(void) {
    return mixer.initialized;
}

int mixer_play(const uint8_t *data, uint32_t size, const audio_format_t *format,
               uint8_t volume, bool loop, const char *name) {
    if (!mixer.initialized || !data || size == 0 || !format) return -1;

    int ch = mixer_find_free_channel();
    if (ch < 0) {
        serial_puts("mixer: no free channels\n");
        return -1;
    }

    mixer_channel_t *c = &mixer.channels[ch];
    c->data = data;
    c->data_size = size;
    c->position = 0;
    c->format = *format;
    c->volume = volume;
    c->pan = 0;
    c->loop = loop;
    c->name = name;
    c->state = MIXER_CHAN_PLAYING;

    mixer.active_channels++;

    serial_printf("mixer: channel %d playing '%s' (%dHz %dch %dbit, %d bytes)\n",
                  ch, name ? name : "unnamed",
                  format->sample_rate, format->channels, format->bits, size);

    return ch;
}

int mixer_play_wav(const uint8_t *wav_data, uint32_t wav_size,
                   uint8_t volume, bool loop, const char *name) {
    if (!mixer.initialized || !wav_data || wav_size < 44) return -1;

    /* Parse WAV header */
    if (wav_data[0] != 'R' || wav_data[1] != 'I' ||
        wav_data[2] != 'F' || wav_data[3] != 'F') return -1;
    if (wav_data[8] != 'W' || wav_data[9] != 'A' ||
        wav_data[10] != 'V' || wav_data[11] != 'E') return -1;

    uint16_t audio_format = *(uint16_t *)(wav_data + 20);
    if (audio_format != 1) return -1;  /* PCM only */

    audio_format_t fmt;
    fmt.channels    = *(uint16_t *)(wav_data + 22);
    fmt.sample_rate = *(uint32_t *)(wav_data + 24);
    fmt.bits        = *(uint16_t *)(wav_data + 34);

    /* Find data chunk */
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

    if (data_size == 0) return -1;
    if (data_offset + data_size > wav_size)
        data_size = wav_size - data_offset;

    return mixer_play(wav_data + data_offset, data_size, &fmt, volume, loop, name);
}

int mixer_play_tone(uint32_t frequency, uint32_t duration_ms, uint8_t volume) {
    if (!mixer.initialized) return -1;

    uint32_t num_samples = (MIXER_SAMPLE_RATE * duration_ms) / 1000;
    uint32_t data_size = num_samples * 4;  /* stereo 16-bit */

    if (data_size > TONE_MAX_SIZE) data_size = TONE_MAX_SIZE;

    /* Allocate tone buffer if needed */
    if (!tone_buffer) {
        tone_buffer = (uint8_t *)kalloc(TONE_MAX_SIZE);
        if (!tone_buffer) return -1;
    }

    /* Generate square wave */
    int16_t *samples = (int16_t *)tone_buffer;
    uint32_t period = MIXER_SAMPLE_RATE / frequency;
    if (period == 0) period = 1;
    int16_t amplitude = 8000;

    num_samples = data_size / 4;
    for (uint32_t i = 0; i < num_samples; i++) {
        int16_t val = ((i % period) < (period / 2)) ? amplitude : -amplitude;
        samples[i * 2]     = val;
        samples[i * 2 + 1] = val;
    }

    audio_format_t fmt = { .sample_rate = MIXER_SAMPLE_RATE, .channels = 2, .bits = 16 };
    return mixer_play(tone_buffer, data_size, &fmt, volume, false, "tone");
}

void mixer_stop_channel(int channel) {
    if (channel < 0 || channel >= MIXER_MAX_CHANNELS) return;
    mixer_channel_t *c = &mixer.channels[channel];
    if (c->state == MIXER_CHAN_PLAYING || c->state == MIXER_CHAN_PAUSED) {
        c->state = MIXER_CHAN_FREE;
        c->data = NULL;
        c->position = 0;
        if (mixer.active_channels > 0) mixer.active_channels--;
    }
}

void mixer_pause_channel(int channel) {
    if (channel < 0 || channel >= MIXER_MAX_CHANNELS) return;
    if (mixer.channels[channel].state == MIXER_CHAN_PLAYING) {
        mixer.channels[channel].state = MIXER_CHAN_PAUSED;
    }
}

void mixer_resume_channel(int channel) {
    if (channel < 0 || channel >= MIXER_MAX_CHANNELS) return;
    if (mixer.channels[channel].state == MIXER_CHAN_PAUSED) {
        mixer.channels[channel].state = MIXER_CHAN_PLAYING;
    }
}

void mixer_stop_all(void) {
    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) {
        mixer_stop_channel(i);
    }
    mixer.active_channels = 0;
}

void mixer_set_channel_volume(int channel, uint8_t volume) {
    if (channel < 0 || channel >= MIXER_MAX_CHANNELS) return;
    mixer.channels[channel].volume = volume;
}

void mixer_set_channel_pan(int channel, int8_t pan) {
    if (channel < 0 || channel >= MIXER_MAX_CHANNELS) return;
    mixer.channels[channel].pan = pan;
}

void mixer_set_master_volume(uint8_t volume) {
    mixer.master_volume = volume;
}

uint32_t mixer_mix(int16_t *output, uint32_t samples) {
    if (!output || samples == 0) return 0;
    if (samples > MIXER_BUFFER_SAMPLES) samples = MIXER_BUFFER_SAMPLES;

    /* Clear mix accumulators */
    memset(mixer.mix_buffer_l, 0, samples * sizeof(int32_t));
    memset(mixer.mix_buffer_r, 0, samples * sizeof(int32_t));

    /* Mix each active channel */
    for (int ch = 0; ch < MIXER_MAX_CHANNELS; ch++) {
        mixer_channel_t *c = &mixer.channels[ch];
        if (c->state != MIXER_CHAN_PLAYING) continue;

        /* Calculate pan volumes (0-256 range for fixed-point) */
        int32_t pan_l, pan_r;
        if (c->pan <= 0) {
            pan_l = 256;
            pan_r = 256 + (int32_t)c->pan * 2;  /* -128 -> 0, 0 -> 256 */
            if (pan_r < 0) pan_r = 0;
        } else {
            pan_r = 256;
            pan_l = 256 - (int32_t)c->pan * 2;   /* 0 -> 256, 127 -> 2 */
            if (pan_l < 0) pan_l = 0;
        }

        /* Channel volume (0-255) scaled to 0-256 */
        int32_t vol = (int32_t)c->volume + 1;  /* avoid zero */

        /* Simple resampling: nearest-neighbor for different sample rates */
        uint32_t src_rate = c->format.sample_rate;
        uint32_t bytes_per_sample = (c->format.bits / 8) * c->format.channels;

        for (uint32_t i = 0; i < samples; i++) {
            if (c->position >= c->data_size) {
                if (c->loop) {
                    c->position = 0;
                } else {
                    c->state = MIXER_CHAN_DONE;
                    if (mixer.active_channels > 0) mixer.active_channels--;
                    break;
                }
            }

            int16_t left, right;
            channel_read_stereo(c, &left, &right);

            /* Apply channel volume and panning */
            int32_t l = ((int32_t)left  * vol * pan_l) >> 16;
            int32_t r = ((int32_t)right * vol * pan_r) >> 16;

            /* Skip samples for downsampling (simple rate conversion) */
            if (src_rate > MIXER_SAMPLE_RATE && src_rate > 0) {
                /* Source is higher rate - skip extra samples */
                uint32_t skip = (src_rate / MIXER_SAMPLE_RATE) - 1;
                c->position += skip * bytes_per_sample;
            }

            mixer.mix_buffer_l[i] += l;
            mixer.mix_buffer_r[i] += r;
        }
    }

    /* Apply master volume and clamp to output */
    int32_t master = (int32_t)mixer.master_volume + 1;
    for (uint32_t i = 0; i < samples; i++) {
        int32_t l = (mixer.mix_buffer_l[i] * master) >> 8;
        int32_t r = (mixer.mix_buffer_r[i] * master) >> 8;
        output[i * 2]     = clamp16(l);
        output[i * 2 + 1] = clamp16(r);
    }

    return samples;
}

void mixer_poll(void) {
    if (!mixer.initialized) return;

    /* Clean up done channels */
    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) {
        if (mixer.channels[i].state == MIXER_CHAN_DONE) {
            mixer.channels[i].state = MIXER_CHAN_FREE;
            mixer.channels[i].data = NULL;
        }
    }

    /* If AC97 is available and we have active channels, feed the DMA */
    if (ac97_is_available()) {
        ac97_poll();
    }
}

void mixer_get_status(char *buf, size_t buflen) {
    if (!mixer.initialized) {
        snprintf(buf, buflen, "Mixer: Not initialized");
        return;
    }

    int active = 0;
    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) {
        if (mixer.channels[i].state == MIXER_CHAN_PLAYING) active++;
    }

    snprintf(buf, buflen, "Mixer: master=%d%% channels=%d/%d active",
             (mixer.master_volume * 100) / 255, active, MIXER_MAX_CHANNELS);
}

int mixer_active_count(void) {
    int count = 0;
    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) {
        if (mixer.channels[i].state == MIXER_CHAN_PLAYING) count++;
    }
    return count;
}
