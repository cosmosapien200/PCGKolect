#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Audio abstraction layer sitting between the PCM1803A I2S driver and the
 * consumers (USB audio, UART audio, ...).
 *
 * Responsibilities:
 *   - read raw stereo 32-bit frames from the PCM1803A (48 kHz source)
 *   - keep only the left channel (drop the right channel)
 *   - down-sample the source rate to a consumer requested rate (box average)
 *   - convert the 24-bit PCM data to the requested bit depth (24 / 16 / 8)
 *
 * A single producer task owns the I2S channel and fans the mono stream out to
 * any number of independent streams, each with its own sample rate and bit
 * depth. This lets e.g. USB audio read at 48 kHz/16-bit while a UART stream
 * reads the same source at 8 kHz/16-bit.
 */

typedef enum {
    AUDIO_BITS_8  = 8,
    AUDIO_BITS_16 = 16,
    AUDIO_BITS_24 = 24,
} audio_bit_depth_t;

typedef struct {
    /* Desired output sample rate in Hz. Must divide the 48000 Hz source rate
     * evenly (48000, 24000, 16000, 12000, 8000, 6000, 4800, 4000, ...). */
    uint32_t          sample_rate;
    /* Output sample width. 24-bit samples are sign-extended int32_t values. */
    audio_bit_depth_t bits;
} audio_config_t;

typedef struct audio_stream_s *audio_stream_t;

/*
 * Initialize the audio front-end (PCM1803A + internal ring buffer + producer
 * task). Idempotent; called automatically by audio_setup().
 */
esp_err_t audio_init(void);

/*
 * Open a consumer stream at the requested rate/bit depth.
 * Returns NULL on failure (invalid rate/depth or out of memory).
 */
audio_stream_t audio_setup(const audio_config_t *cfg);

/*
 * Blocking read of mono samples in the stream's configured format.
 * Fills up to `size` bytes and stores the number of bytes produced in
 * `bytes_read`. Returns ESP_OK on success. A stalled producer blocks the
 * caller (same behaviour as pcm1803_read()).
 */
esp_err_t audio_read(
    audio_stream_t stream,
    void *buf,
    size_t size,
    size_t *bytes_read
);

/* Release a stream opened with audio_setup(). */
void audio_stream_close(audio_stream_t stream);

#endif
