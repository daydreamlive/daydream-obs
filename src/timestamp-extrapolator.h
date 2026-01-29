/*
 * Timestamp Extrapolator - Based on Chrome WebRTC implementation
 * RLS filter for mapping RTP timestamps to local clock time
 */

#ifndef TIMESTAMP_EXTRAPOLATOR_H
#define TIMESTAMP_EXTRAPOLATOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct timestamp_extrapolator timestamp_extrapolator_t;

// Create a new timestamp extrapolator
timestamp_extrapolator_t *timestamp_extrapolator_create(void);

// Destroy the extrapolator
void timestamp_extrapolator_destroy(timestamp_extrapolator_t *te);

// Reset to initial state
void timestamp_extrapolator_reset(timestamp_extrapolator_t *te, uint64_t start_time_us);

// Update with new RTP timestamp / local time pair
void timestamp_extrapolator_update(timestamp_extrapolator_t *te, uint64_t now_us, uint32_t rtp_timestamp);

// Extrapolate local time for an RTP timestamp (returns 0 if not ready)
uint64_t timestamp_extrapolator_extrapolate(timestamp_extrapolator_t *te, uint32_t rtp_timestamp);

// Check if extrapolator is ready (has enough samples)
bool timestamp_extrapolator_is_ready(timestamp_extrapolator_t *te);

#ifdef __cplusplus
}
#endif

#endif // TIMESTAMP_EXTRAPOLATOR_H
