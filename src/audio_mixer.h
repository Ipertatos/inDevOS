/**
 * @file audio_mixer.h
 * @brief Software audio mixer for multiple simultaneous audio streams
 *
 * Provides a mixing layer that combines multiple audio sources into a single
 * output stream for the AC'97 DMA engine. Supports volume per-channel,
 * panning, and automatic sample rate conversion.
 */

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include "types.h"

/** Maximum number of simultaneous mixer channels */
#define MIXER_MAX_CHANNELS  8

/** Mixer output buffer size (samples, not bytes) */
#define MIXER_BUFFER_SAMPLES 2048

/** Sample rate for the mixer output */
#define MIXER_SAMPLE_RATE   44100

/** Channel state */
typedef enum {
    MIXER_CHAN_FREE = 0,    /**< Channel is available */
    MIXER_CHAN_PLAYING,     /**< Channel is actively playing */
    MIXER_CHAN_PAUSED,      /**< Channel is paused */
    MIXER_CHAN_DONE         /**< Channel finished playback */
} mixer_chan_state_t;

/** Audio format descriptor */
typedef struct {
    uint32_t sample_rate;   /**< Sample rate in Hz */
    uint16_t channels;      /**< Number of channels (1=mono, 2=stereo) */
    uint16_t bits;          /**< Bits per sample (8 or 16) */
} audio_format_t;

/** Mixer channel - represents one audio stream */
typedef struct {
    mixer_chan_state_t state;    /**< Current state */
    const uint8_t *data;        /**< Pointer to audio PCM data */
    uint32_t data_size;         /**< Total data size in bytes */
    uint32_t position;          /**< Current playback position (bytes) */
    audio_format_t format;      /**< Audio format */
    uint8_t volume;             /**< Channel volume (0-255) */
    int8_t pan;                 /**< Stereo panning (-128=left, 0=center, 127=right) */
    bool loop;                  /**< Loop playback when reaching end */
    const char *name;           /**< Optional channel name for debugging */
} mixer_channel_t;

/** Mixer state */
typedef struct {
    bool initialized;
    mixer_channel_t channels[MIXER_MAX_CHANNELS];
    uint8_t master_volume;      /**< Master volume (0-255) */
    int32_t mix_buffer_l[MIXER_BUFFER_SAMPLES]; /**< Left channel mix accumulator */
    int32_t mix_buffer_r[MIXER_BUFFER_SAMPLES]; /**< Right channel mix accumulator */
    uint32_t active_channels;   /**< Number of active channels */
} audio_mixer_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize the audio mixer
 * @return 0 on success, -1 on error
 */
int mixer_init(void);

/**
 * @brief Check if mixer is available
 * @return true if initialized
 */
bool mixer_is_available(void);

/**
 * @brief Play a PCM audio buffer on the next available channel
 *
 * @param data      PCM audio data
 * @param size      Data size in bytes
 * @param format    Audio format descriptor
 * @param volume    Channel volume (0-255, 255=full)
 * @param loop      Whether to loop playback
 * @param name      Optional name for debugging
 * @return Channel index (0-7) on success, -1 if no free channels
 */
int mixer_play(const uint8_t *data, uint32_t size, const audio_format_t *format,
               uint8_t volume, bool loop, const char *name);

/**
 * @brief Play a WAV file buffer on the next available channel
 *
 * Parses WAV header automatically and sets up the channel.
 *
 * @param wav_data  WAV file data (with header)
 * @param wav_size  Total WAV file size
 * @param volume    Channel volume (0-255)
 * @param loop      Whether to loop
 * @param name      Optional name
 * @return Channel index on success, -1 on error
 */
int mixer_play_wav(const uint8_t *wav_data, uint32_t wav_size,
                   uint8_t volume, bool loop, const char *name);

/**
 * @brief Generate and play a tone on a mixer channel
 *
 * @param frequency  Tone frequency in Hz
 * @param duration_ms Duration in milliseconds
 * @param volume     Volume (0-255)
 * @return Channel index on success, -1 on error
 */
int mixer_play_tone(uint32_t frequency, uint32_t duration_ms, uint8_t volume);

/**
 * @brief Stop a specific channel
 * @param channel Channel index
 */
void mixer_stop_channel(int channel);

/**
 * @brief Pause a specific channel
 * @param channel Channel index
 */
void mixer_pause_channel(int channel);

/**
 * @brief Resume a paused channel
 * @param channel Channel index
 */
void mixer_resume_channel(int channel);

/**
 * @brief Stop all channels
 */
void mixer_stop_all(void);

/**
 * @brief Set volume for a specific channel
 * @param channel Channel index
 * @param volume  Volume (0-255)
 */
void mixer_set_channel_volume(int channel, uint8_t volume);

/**
 * @brief Set panning for a specific channel
 * @param channel Channel index
 * @param pan     Panning (-128=left, 0=center, 127=right)
 */
void mixer_set_channel_pan(int channel, int8_t pan);

/**
 * @brief Set the master volume
 * @param volume Master volume (0-255)
 */
void mixer_set_master_volume(uint8_t volume);

/**
 * @brief Mix all active channels and produce output samples
 *
 * This is the core mixing function. It reads samples from all active
 * channels, mixes them together with volume/panning, and writes the
 * result to the output buffer. Called by the AC'97 DMA refill path.
 *
 * @param output    Output buffer (16-bit signed stereo interleaved)
 * @param samples   Number of stereo sample pairs to generate
 * @return Number of samples actually generated
 */
uint32_t mixer_mix(int16_t *output, uint32_t samples);

/**
 * @brief Poll the mixer - checks for completed channels, feeds AC97
 *
 * Should be called periodically from the main loop or timer.
 */
void mixer_poll(void);

/**
 * @brief Get status string for the mixer
 * @param buf    Output buffer
 * @param buflen Buffer size
 */
void mixer_get_status(char *buf, size_t buflen);

/**
 * @brief Get the number of currently active channels
 * @return Number of channels in PLAYING state
 */
int mixer_active_count(void);

#endif /* AUDIO_MIXER_H */
