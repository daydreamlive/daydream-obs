#include "daydream-frameskip.h"
#include <stdlib.h>

struct daydream_frameskip {
	bool enabled;
	uint32_t last_displayed_rtp_ts;
	bool sync_established;
	uint64_t frames_received;
	uint64_t frames_skipped;
};

struct daydream_frameskip *daydream_frameskip_create(bool enabled)
{
	struct daydream_frameskip *fs = calloc(1, sizeof(struct daydream_frameskip));
	if (!fs)
		return NULL;

	fs->enabled = enabled;
	fs->sync_established = false;
	fs->last_displayed_rtp_ts = 0;
	fs->frames_received = 0;
	fs->frames_skipped = 0;

	return fs;
}

void daydream_frameskip_destroy(struct daydream_frameskip *fs)
{
	free(fs);
}

bool daydream_frameskip_should_display(struct daydream_frameskip *fs, uint32_t rtp_timestamp)
{
	if (!fs)
		return true;

	fs->frames_received++;

	if (!fs->enabled)
		return true;

	if (!fs->sync_established) {
		// First frame after reset - establish sync point
		fs->last_displayed_rtp_ts = rtp_timestamp;
		fs->sync_established = true;
		return true;
	}

	// Check if frame is out of order (older than last displayed)
	// Calculate "forward distance" - how far ahead this timestamp is from last.
	// Due to unsigned wraparound, this handles the case where rtp_timestamp < last
	// but is actually newer (after wraparound).
	// If forward_distance > 0x80000000, the frame is actually "behind" us.
	uint32_t forward_distance = rtp_timestamp - fs->last_displayed_rtp_ts;
	if (forward_distance == 0 || forward_distance > 0x80000000) {
		// Frame is duplicate or older than last displayed - skip it
		fs->frames_skipped++;
		return false;
	}

	// Frame is newer - display it
	fs->last_displayed_rtp_ts = rtp_timestamp;
	return true;
}

void daydream_frameskip_set_enabled(struct daydream_frameskip *fs, bool enabled)
{
	if (fs)
		fs->enabled = enabled;
}

bool daydream_frameskip_is_enabled(struct daydream_frameskip *fs)
{
	return fs ? fs->enabled : false;
}

void daydream_frameskip_reset(struct daydream_frameskip *fs)
{
	if (!fs)
		return;

	fs->sync_established = false;
	fs->last_displayed_rtp_ts = 0;
	fs->frames_received = 0;
	fs->frames_skipped = 0;
}

void daydream_frameskip_get_stats(struct daydream_frameskip *fs, struct daydream_frameskip_stats *stats)
{
	if (!fs || !stats)
		return;

	stats->frames_received = fs->frames_received;
	stats->frames_skipped = fs->frames_skipped;
	stats->sync_established = fs->sync_established;
	stats->last_rtp_timestamp = fs->last_displayed_rtp_ts;
}
