#pragma once

#include <stdbool.h>
#include <stdint.h>

// Opaque handle for frame skip filter
struct daydream_frameskip;

// Statistics for monitoring
struct daydream_frameskip_stats {
	uint64_t frames_received;
	uint64_t frames_skipped;
	bool sync_established;
	uint32_t last_rtp_timestamp;
};

// Create a new frame skip filter
// enabled: whether frame skipping is active
struct daydream_frameskip *daydream_frameskip_create(bool enabled);

// Destroy frame skip filter
void daydream_frameskip_destroy(struct daydream_frameskip *fs);

// Check if a frame should be displayed based on its RTP timestamp
// Returns true if frame should be displayed, false if it should be skipped
// Automatically tracks timestamps and handles wraparound
bool daydream_frameskip_should_display(struct daydream_frameskip *fs, uint32_t rtp_timestamp);

// Enable or disable frame skipping
void daydream_frameskip_set_enabled(struct daydream_frameskip *fs, bool enabled);

// Check if frame skipping is enabled
bool daydream_frameskip_is_enabled(struct daydream_frameskip *fs);

// Reset synchronization state (call when stream restarts)
void daydream_frameskip_reset(struct daydream_frameskip *fs);

// Get current statistics
void daydream_frameskip_get_stats(struct daydream_frameskip *fs, struct daydream_frameskip_stats *stats);
