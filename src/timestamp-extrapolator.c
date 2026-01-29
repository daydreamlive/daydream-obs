/*
 * Timestamp Extrapolator - Based on Chrome WebRTC implementation
 * Recursive Least Squares filter for RTP to local time mapping
 */

#include "timestamp-extrapolator.h"
#include <obs-module.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Constants from Chrome WebRTC
#define STARTUP_FILTER_DELAY 2          // Packets before trusting extrapolation
#define LAMBDA 1.0                      // RLS forgetting factor
#define P00_INITIAL 1.0                 // Initial covariance for slope
#define P11_INITIAL 1e10                // Initial covariance for offset (high uncertainty)
#define HARD_RESET_TIMEOUT_US 10000000  // 10 seconds without update = reset
#define RTP_CLOCK_RATE 90.0             // 90kHz = 90 ticks per ms

struct timestamp_extrapolator {
	// RLS filter state: y = w[0]*t + w[1]
	// w[0] = slope (should be ~90 for 90kHz RTP clock)
	// w[1] = offset
	double w[2];

	// Covariance matrix P
	double p[2][2];

	// Timestamps
	uint64_t start_time_us;
	uint64_t prev_time_us;

	// RTP timestamp tracking (with unwrapping)
	int64_t first_unwrapped_rtp;
	int64_t prev_unwrapped_rtp;
	bool has_first_rtp;

	// Packet counter for startup delay
	int packet_count;

	// For RTP timestamp unwrapping
	uint32_t prev_rtp;
	int64_t unwrap_offset;
};

// Unwrap RTP timestamp (handle 32-bit wraparound)
static int64_t unwrap_rtp(timestamp_extrapolator_t *te, uint32_t rtp)
{
	if (!te->has_first_rtp) {
		te->prev_rtp = rtp;
		te->unwrap_offset = 0;
		return (int64_t)rtp;
	}

	// Detect wraparound
	int32_t diff = (int32_t)(rtp - te->prev_rtp);

	// If diff is very negative, we wrapped forward
	// If diff is very positive, we might have wrapped backward (reordering)
	if (diff < -0x40000000) {
		// Forward wrap
		te->unwrap_offset += 0x100000000LL;
	} else if (diff > 0x40000000) {
		// Backward wrap (rare, usually reordering)
		te->unwrap_offset -= 0x100000000LL;
	}

	te->prev_rtp = rtp;
	return (int64_t)rtp + te->unwrap_offset;
}

timestamp_extrapolator_t *timestamp_extrapolator_create(void)
{
	timestamp_extrapolator_t *te = calloc(1, sizeof(timestamp_extrapolator_t));
	if (te) {
		timestamp_extrapolator_reset(te, 0);
	}
	return te;
}

void timestamp_extrapolator_destroy(timestamp_extrapolator_t *te)
{
	free(te);
}

void timestamp_extrapolator_reset(timestamp_extrapolator_t *te, uint64_t start_time_us)
{
	if (!te)
		return;

	te->start_time_us = start_time_us;
	te->prev_time_us = start_time_us;

	// Initialize RLS: slope = 90 (90kHz), offset = 0
	te->w[0] = RTP_CLOCK_RATE;
	te->w[1] = 0.0;

	// Initial covariance
	te->p[0][0] = P00_INITIAL;
	te->p[0][1] = 0.0;
	te->p[1][0] = 0.0;
	te->p[1][1] = P11_INITIAL;

	// Reset RTP tracking
	te->first_unwrapped_rtp = 0;
	te->prev_unwrapped_rtp = 0;
	te->has_first_rtp = false;
	te->prev_rtp = 0;
	te->unwrap_offset = 0;

	te->packet_count = 0;
}

void timestamp_extrapolator_update(timestamp_extrapolator_t *te, uint64_t now_us, uint32_t rtp_timestamp)
{
	if (!te)
		return;

	// Hard reset on timeout
	if (te->prev_time_us > 0 && (now_us - te->prev_time_us) > HARD_RESET_TIMEOUT_US) {
		blog(LOG_INFO, "[TS Extrap] Hard reset due to timeout");
		timestamp_extrapolator_reset(te, now_us);
	}
	te->prev_time_us = now_us;

	// Unwrap RTP timestamp
	int64_t unwrapped_rtp = unwrap_rtp(te, rtp_timestamp);

	if (!te->has_first_rtp) {
		te->first_unwrapped_rtp = unwrapped_rtp;
		te->has_first_rtp = true;
		// Initialize offset based on first sample
		double t_ms = (double)(now_us - te->start_time_us) / 1000.0;
		te->w[1] = -te->w[0] * t_ms;
	}

	// Time since start in milliseconds (offset removed for numerical stability)
	double t_ms = (double)(now_us - te->start_time_us) / 1000.0;

	// Calculate residual: actual_rtp - predicted_rtp
	double predicted_rtp = t_ms * te->w[0] + te->w[1];
	double actual_rtp = (double)(unwrapped_rtp - te->first_unwrapped_rtp);
	double residual = actual_rtp - predicted_rtp;

	// Skip reordered packets
	if (te->prev_unwrapped_rtp > 0 && unwrapped_rtp < te->prev_unwrapped_rtp) {
		return;
	}

	// RLS update
	// K = P*T / (lambda + T'*P*T)  where T = [t_ms, 1]'
	double K[2];
	K[0] = te->p[0][0] * t_ms + te->p[0][1];
	K[1] = te->p[1][0] * t_ms + te->p[1][1];
	double TPT = LAMBDA + t_ms * K[0] + K[1];
	K[0] /= TPT;
	K[1] /= TPT;

	// Update weights: w = w + K * residual
	te->w[0] += K[0] * residual;
	te->w[1] += K[1] * residual;

	// Update covariance: P = (P - K*T'*P) / lambda
	double p00 = (te->p[0][0] - K[0] * (t_ms * te->p[0][0] + te->p[1][0])) / LAMBDA;
	double p01 = (te->p[0][1] - K[0] * (t_ms * te->p[0][1] + te->p[1][1])) / LAMBDA;
	te->p[1][0] = (te->p[1][0] - K[1] * (t_ms * te->p[0][0] + te->p[1][0])) / LAMBDA;
	te->p[1][1] = (te->p[1][1] - K[1] * (t_ms * te->p[0][1] + te->p[1][1])) / LAMBDA;
	te->p[0][0] = p00;
	te->p[0][1] = p01;

	te->prev_unwrapped_rtp = unwrapped_rtp;
	te->packet_count++;
}

uint64_t timestamp_extrapolator_extrapolate(timestamp_extrapolator_t *te, uint32_t rtp_timestamp)
{
	if (!te || !te->has_first_rtp)
		return 0;

	// During startup, use simple linear extrapolation from previous sample
	if (te->packet_count < STARTUP_FILTER_DELAY) {
		// Simple: assume constant RTP rate
		int64_t unwrapped = unwrap_rtp(te, rtp_timestamp);
		int64_t rtp_diff = unwrapped - te->prev_unwrapped_rtp;
		double ms_diff = (double)rtp_diff / RTP_CLOCK_RATE;
		return te->prev_time_us + (uint64_t)(ms_diff * 1000.0);
	}

	// Extrapolate: given rtp, find t such that rtp = w[0]*t + w[1]
	// t = (rtp - w[1]) / w[0]
	if (te->w[0] < 1e-3) {
		return te->start_time_us; // Degenerate case
	}

	// Peek unwrap (don't modify state)
	int64_t unwrapped = (int64_t)rtp_timestamp + te->unwrap_offset;
	int32_t diff = (int32_t)(rtp_timestamp - te->prev_rtp);
	if (diff < -0x40000000) {
		unwrapped += 0x100000000LL;
	} else if (diff > 0x40000000) {
		unwrapped -= 0x100000000LL;
	}

	double rtp_since_first = (double)(unwrapped - te->first_unwrapped_rtp);
	double t_ms = (rtp_since_first - te->w[1]) / te->w[0];

	if (t_ms < 0)
		t_ms = 0;

	return te->start_time_us + (uint64_t)(t_ms * 1000.0);
}

bool timestamp_extrapolator_is_ready(timestamp_extrapolator_t *te)
{
	return te && te->packet_count >= STARTUP_FILTER_DELAY;
}
