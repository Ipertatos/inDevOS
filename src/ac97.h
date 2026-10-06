/**
 * @file ac97.h
 * @brief Intel AC'97 Audio Codec driver
 *
 * Implements the AC'97 audio standard for digital audio playback.
 * Supports PCI-based AC'97 controllers (Intel ICH series).
 * Uses DMA-based playback via the Bus Master engine.
 */

#ifndef AC97_H
#define AC97_H

#include "types.h"

/* ============================================================================
 * AC'97 Native Audio Mixer (NAM) Registers (Codec registers via BAR0)
 * ============================================================================ */
#define AC97_NAM_RESET          0x00    /**< Reset codec */
#define AC97_NAM_MASTER_VOL     0x02    /**< Master output volume */
#define AC97_NAM_AUX_OUT_VOL    0x04    /**< Aux/headphone volume */
#define AC97_NAM_MONO_VOL       0x06    /**< Mono output volume */
#define AC97_NAM_PC_BEEP_VOL    0x0A    /**< PC beep volume */
#define AC97_NAM_PHONE_VOL      0x0C    /**< Phone volume */
#define AC97_NAM_MIC_VOL        0x0E    /**< Mic input volume */
#define AC97_NAM_LINE_IN_VOL    0x10    /**< Line-in volume */
#define AC97_NAM_CD_VOL         0x12    /**< CD volume */
#define AC97_NAM_PCM_OUT_VOL    0x18    /**< PCM output volume */
#define AC97_NAM_RECORD_SEL     0x1A    /**< Record source select */
#define AC97_NAM_RECORD_GAIN    0x1C    /**< Record gain */
#define AC97_NAM_GENERAL_PURP   0x20    /**< General purpose */
#define AC97_NAM_EXTENDED_ID    0x28    /**< Extended audio ID */
#define AC97_NAM_EXTENDED_CTRL  0x2A    /**< Extended audio status/ctrl */
#define AC97_NAM_FRONT_DAC_RATE 0x2C    /**< Front DAC sample rate */
#define AC97_NAM_LR_ADC_RATE    0x32    /**< L/R ADC sample rate */
#define AC97_NAM_VENDOR_ID1     0x7C    /**< Vendor ID 1 */
#define AC97_NAM_VENDOR_ID2     0x7E    /**< Vendor ID 2 */

/* ============================================================================
 * AC'97 Native Audio Bus Master (NABM) Registers (DMA via BAR1)
 * ============================================================================ */

/* PCM Input (record) channel */
#define AC97_NABM_PI_BDBAR      0x00    /**< Buffer Descriptor Base Address */
#define AC97_NABM_PI_CIV        0x04    /**< Current Index Value */
#define AC97_NABM_PI_LVI        0x05    /**< Last Valid Index */
#define AC97_NABM_PI_SR         0x06    /**< Status Register */
#define AC97_NABM_PI_PICB       0x08    /**< Position In Current Buffer */
#define AC97_NABM_PI_CR         0x0B    /**< Control Register */

/* PCM Output (playback) channel */
#define AC97_NABM_PO_BDBAR      0x10    /**< Buffer Descriptor Base Address */
#define AC97_NABM_PO_CIV        0x14    /**< Current Index Value (8-bit) */
#define AC97_NABM_PO_LVI        0x15    /**< Last Valid Index (8-bit) */
#define AC97_NABM_PO_SR         0x16    /**< Status Register (16-bit) */
#define AC97_NABM_PO_PICB       0x18    /**< Position In Current Buffer (16-bit) */
#define AC97_NABM_PO_CR         0x1B    /**< Control Register (8-bit) */

/* Mic Input channel */
#define AC97_NABM_MC_BDBAR      0x20
#define AC97_NABM_MC_CIV        0x24
#define AC97_NABM_MC_LVI        0x25
#define AC97_NABM_MC_SR         0x26
#define AC97_NABM_MC_PICB       0x28
#define AC97_NABM_MC_CR         0x2B

/* Global Control */
#define AC97_NABM_GLB_CTRL      0x2C    /**< Global Control (32-bit) */
#define AC97_NABM_GLB_STS       0x30    /**< Global Status (32-bit) */

/* Control Register bits */
#define AC97_CR_RPBM            (1 << 0)    /**< Run/Pause Bus Master */
#define AC97_CR_RR              (1 << 1)    /**< Reset Registers */
#define AC97_CR_LVBIE           (1 << 2)    /**< Last Valid Buffer Interrupt Enable */
#define AC97_CR_FEIE            (1 << 3)    /**< FIFO Error Interrupt Enable */
#define AC97_CR_IOCE            (1 << 4)    /**< Interrupt On Completion Enable */

/* Status Register bits */
#define AC97_SR_DCH             (1 << 0)    /**< DMA Controller Halted */
#define AC97_SR_CELV            (1 << 1)    /**< Current Equals Last Valid */
#define AC97_SR_LVBCI           (1 << 2)    /**< Last Valid Buffer Completion Interrupt */
#define AC97_SR_BCIS            (1 << 3)    /**< Buffer Completion Interrupt Status */
#define AC97_SR_FIFOE           (1 << 4)    /**< FIFO Error */

/* Global Control bits */
#define AC97_GC_GIE             (1 << 0)    /**< GPI Interrupt Enable */
#define AC97_GC_COLD_RESET      (1 << 1)    /**< Cold Reset */
#define AC97_GC_WARM_RESET      (1 << 2)    /**< Warm Reset */
#define AC97_GC_SHUT            (1 << 3)    /**< Shut off (ACLink off) */
#define AC97_GC_2CH_MODE        (0 << 20)   /**< 2-channel mode */
#define AC97_GC_4CH_MODE        (1 << 20)   /**< 4-channel mode */
#define AC97_GC_6CH_MODE        (2 << 20)   /**< 6-channel mode */

/* Global Status bits */
#define AC97_GS_MD3             (1 << 1)    /**< Modem Codec Ready */
#define AC97_GS_AD3             (1 << 2)    /**< Audio Codec Ready */

/* ============================================================================
 * Buffer Descriptor (BD) structure - 8 bytes each, 32 entries max
 * ============================================================================ */
#define AC97_BDL_MAX_ENTRIES    32

/**
 * @brief AC'97 Buffer Descriptor entry
 *
 * Each entry in the Buffer Descriptor List (BDL) describes one DMA buffer.
 * The hardware walks through these entries in order, playing each buffer.
 */
typedef struct __attribute__((packed)) {
    uint32_t buffer_addr;   /**< Physical address of the audio data buffer */
    uint16_t length;        /**< Number of samples (NOT bytes) */
    uint16_t flags;         /**< BIT 15: IOC, BIT 14: BUP */
} ac97_buffer_desc_t;

/** @brief Buffer descriptor flags */
#define AC97_BD_IOC     (1 << 15)   /**< Interrupt On Completion */
#define AC97_BD_BUP     (1 << 14)   /**< Buffer Underrun Policy (1=last sample, 0=zero) */

/* ============================================================================
 * Driver state
 * ============================================================================ */

/** @brief Maximum number of audio buffers for double-buffering */
#define AC97_NUM_BUFFERS        2

/** @brief Size of each audio buffer in bytes (8KB = ~46ms at 44100Hz stereo 16-bit) */
#define AC97_BUFFER_SIZE        8192

/** @brief Default sample rate */
#define AC97_DEFAULT_SAMPLE_RATE 44100

/**
 * @brief AC'97 driver state structure
 */
typedef struct {
    bool initialized;               /**< Driver is initialized */
    bool playing;                   /**< Currently playing audio */
    uint16_t nam_port;              /**< NAM I/O base port (BAR0) */
    uint16_t nabm_port;             /**< NABM I/O base port (BAR1) */
    uint32_t sample_rate;           /**< Current sample rate (Hz) */
    uint8_t volume;                 /**< Master volume (0-100) */
    
    /* DMA buffers */
    ac97_buffer_desc_t *bdl;        /**< Buffer Descriptor List (aligned) */
    uint64_t bdl_phys;              /**< Physical address of BDL */
    uint8_t *buffers[AC97_NUM_BUFFERS]; /**< Audio data buffers */
    uint64_t buffer_phys[AC97_NUM_BUFFERS]; /**< Physical addresses of buffers */
    uint32_t current_buffer;        /**< Current buffer being filled */
    
    /* Playback state */
    const uint8_t *play_data;       /**< Data being played */
    uint32_t play_size;             /**< Total size of data */
    uint32_t play_pos;              /**< Current position in data */
} ac97_state_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize the AC'97 audio driver
 *
 * Scans PCI for AC'97 controllers, configures the codec, and sets up DMA.
 *
 * @return 0 on success, -1 if no AC'97 device found
 */
int ac97_init(void);

/**
 * @brief Check if AC'97 driver is available
 * @return true if initialized and ready
 */
bool ac97_is_available(void);

/**
 * @brief Set the master volume
 * @param volume Volume level (0 = mute, 100 = max)
 */
void ac97_set_volume(uint8_t volume);

/**
 * @brief Set the sample rate
 * @param rate Sample rate in Hz (e.g. 44100, 48000)
 * @return Actual sample rate set by the codec
 */
uint32_t ac97_set_sample_rate(uint32_t rate);

/**
 * @brief Play raw PCM audio data
 *
 * Plays 16-bit signed stereo PCM data at the configured sample rate.
 * This function is non-blocking; it sets up DMA and returns.
 *
 * @param data    Pointer to PCM audio data
 * @param size    Size of data in bytes
 * @return 0 on success, -1 on error
 */
int ac97_play_pcm(const uint8_t *data, uint32_t size);

/**
 * @brief Play a WAV file
 *
 * Parses the WAV header, configures the sample rate, and plays the audio.
 *
 * @param wav_data  Pointer to WAV file data (including header)
 * @param wav_size  Total size of WAV file
 * @return 0 on success, -1 on error
 */
int ac97_play_wav(const uint8_t *wav_data, uint32_t wav_size);

/**
 * @brief Stop audio playback
 */
void ac97_stop(void);

/**
 * @brief Resume paused audio playback
 */
void ac97_resume(void);

/**
 * @brief Check if audio is currently playing
 * @return true if playback is active
 */
bool ac97_is_playing(void);

/**
 * @brief Feed more data to the DMA engine (called from interrupt or polling)
 *
 * Checks if a buffer has completed and fills the next one with data.
 * Should be called periodically or from the AC'97 interrupt handler.
 */
void ac97_poll(void);

/**
 * @brief Generate and play a test tone
 * @param frequency  Tone frequency in Hz
 * @param duration_ms Duration in milliseconds
 */
void ac97_test_tone(uint32_t frequency, uint32_t duration_ms);

/**
 * @brief Get a human-readable status string for the AC'97 driver
 * @param buf    Output buffer
 * @param buflen Buffer length
 */
void ac97_get_status(char *buf, size_t buflen);

#endif /* AC97_H */
