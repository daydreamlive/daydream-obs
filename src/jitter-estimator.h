/*
 * Jitter Estimator - Based on Chrome WebRTC implementation
 * Adapted for OBS plugin use
 */

#ifndef JITTER_ESTIMATOR_H
#define JITTER_ESTIMATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jitter_estimator jitter_estimator_t;

// Create a new jitter estimator
jitter_estimator_t *jitter_estimator_create(void);

// Destroy the jitter estimator
void jitter_estimator_destroy(jitter_estimator_t *je);

// Reset to initial state
void jitter_estimator_reset(jitter_estimator_t *je);

// Update with new frame measurement (simple version)
// frame_delay_ms: delay since last frame (can be negative for early frames)
// frame_size: size of current frame in bytes
void jitter_estimator_update(jitter_estimator_t *je, double frame_delay_ms, size_t frame_size);

// Update with RTP timestamp (more accurate - uses inter-frame delay variation)
// rtp_timestamp: RTP timestamp of current frame (90kHz clock)
// receive_time_us: local receive time in microseconds
// frame_size: size of current frame in bytes
void jitter_estimator_update_rtp(jitter_estimator_t *je, uint32_t rtp_timestamp, uint64_t receive_time_us,
				 size_t frame_size);

// Get current jitter estimate in milliseconds
double jitter_estimator_get_ms(jitter_estimator_t *je);

// Get recommended buffer target in frames
int jitter_estimator_get_buffer_target(jitter_estimator_t *je, double fps);

// Get current estimated frame rate
double jitter_estimator_get_fps(jitter_estimator_t *je);

#ifdef __cplusplus
}
#endif

#endif // JITTER_ESTIMATOR_H
