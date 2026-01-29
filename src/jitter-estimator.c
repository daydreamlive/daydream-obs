/*
 * Jitter Estimator - Based on Chrome WebRTC implementation
 * Simplified Kalman filter approach for OBS plugin
 */

#include "jitter-estimator.h"
#include <obs-module.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <sys/time.h>
#endif

// Constants from Chrome WebRTC
#define STARTUP_COUNT 30           // Frames before post-processing
#define ALPHA_COUNT_MAX 400        // Max for noise filter rampup
#define PHI 0.97                   // Time constant for avg frame size
#define PSI 0.9999                 // Time constant for max frame size
#define NUM_STDDEV_DELAY_CLAMP 3.5
#define NUM_STDDEV_DELAY_OUTLIER 15.0
#define NUM_STDDEV_SIZE_OUTLIER 3.0
#define CONGESTION_REJECTION_FACTOR (-0.25)
#define NOISE_STDDEVS 2.33
#define NOISE_STDDEV_OFFSET 30.0
#define MIN_JITTER_MS 1.0
#define MAX_JITTER_MS 10000.0
#define OS_JITTER_MS 10.0
#define FPS_WINDOW 30

// Simple Kalman filter state
typedef struct {
	double slope;         // Frame size to delay slope
	double intercept;     // Base delay offset
	double slope_var;     // Variance of slope estimate
	double intercept_var; // Variance of intercept estimate
	double covariance;    // Covariance between slope and intercept
} kalman_state_t;

// CUSUM detection constants (from Chrome WebRTC timestamp_extrapolator.cc)
#define CUSUM_ALARM_THRESHOLD 60000  // ~666ms in RTP ticks (90kHz)
#define CUSUM_ACC_DRIFT 6600         // ~73ms acceptable drift per frame
#define CUSUM_ACC_MAX_ERROR 7000     // ~77ms max single error

// Max-gap tracking for bursty traffic (AI video specific)
#define MAX_GAP_WINDOW 30            // Track max gap over last N frames
#define BUFFER_TARGET_HYSTERESIS 3   // Require 3+ frame difference to change
#define MIN_BUFFER_TARGET 4          // Never go below 4 frames
#define MAX_BUFFER_TARGET 30         // Never go above 30 frames

struct jitter_estimator {
	// Kalman filter
	kalman_state_t kalman;

	// Frame size statistics
	double avg_frame_size;
	double max_frame_size;
	double var_frame_size;

	// Noise statistics
	double avg_noise_ms;
	double var_noise_ms2;

	// State
	size_t startup_count;
	size_t alpha_count;
	double prev_frame_size;
	bool has_prev_frame;
	double prev_estimate;
	double filtered_estimate;

	// FPS estimation
	double frame_times[FPS_WINDOW];
	int fps_index;
	int fps_count;
	uint64_t last_update_time_us;

	// Inter-frame delay variation (IFDV) tracking
	uint32_t prev_rtp_timestamp;
	uint64_t prev_receive_time_us;
	bool has_prev_rtp;

	// CUSUM delay change detection
	double cusum_pos;
	double cusum_neg;
	int cusum_alarm_count;

	// Max-gap tracking for bursty AI video
	double gap_history[MAX_GAP_WINDOW];
	int gap_index;
	int gap_count;
	double max_gap_ms;          // Current max gap in window
	double smoothed_max_gap_ms; // EMA smoothed max gap
	int last_buffer_target;     // For hysteresis
};

static uint64_t get_time_us(void)
{
#if defined(_WIN32)
	LARGE_INTEGER freq, counter;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	return (uint64_t)(counter.QuadPart * 1000000 / freq.QuadPart);
#elif defined(__APPLE__)
	static mach_timebase_info_data_t timebase = {0};
	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	uint64_t time = mach_absolute_time();
	return time * timebase.numer / timebase.denom / 1000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

static void kalman_reset(kalman_state_t *k)
{
	k->slope = 0.0;
	k->intercept = 0.0;
	k->slope_var = 1e10; // High initial uncertainty
	k->intercept_var = 1e10;
	k->covariance = 0.0;
}

static double kalman_predict(kalman_state_t *k, double delta_frame_size)
{
	return k->slope * delta_frame_size + k->intercept;
}

static void kalman_update(kalman_state_t *k, double frame_delay_ms, double delta_frame_size, double max_frame_size,
			  double var_noise)
{
	// Simplified Kalman update
	// Process noise
	double q_slope = 1e-4 * max_frame_size * max_frame_size;
	double q_intercept = 1e-2;

	k->slope_var += q_slope;
	k->intercept_var += q_intercept;

	// Measurement update
	double h[2] = {delta_frame_size, 1.0}; // Observation matrix
	double S = h[0] * h[0] * k->slope_var + 2 * h[0] * h[1] * k->covariance + h[1] * h[1] * k->intercept_var +
		   var_noise;

	if (S < 1e-6)
		S = 1e-6; // Prevent division by zero

	// Kalman gains
	double K0 = (k->slope_var * h[0] + k->covariance * h[1]) / S;
	double K1 = (k->covariance * h[0] + k->intercept_var * h[1]) / S;

	// Innovation (measurement residual)
	double y = frame_delay_ms - kalman_predict(k, delta_frame_size);

	// State update
	k->slope += K0 * y;
	k->intercept += K1 * y;

	// Covariance update (simplified Joseph form)
	double new_slope_var = (1 - K0 * h[0]) * k->slope_var - K0 * h[1] * k->covariance;
	double new_intercept_var = (1 - K1 * h[1]) * k->intercept_var - K1 * h[0] * k->covariance;
	double new_covariance = -K0 * h[1] * k->intercept_var + (1 - K0 * h[0]) * k->covariance;

	k->slope_var = fmax(new_slope_var, 1.0);
	k->intercept_var = fmax(new_intercept_var, 1.0);
	k->covariance = new_covariance;
}

jitter_estimator_t *jitter_estimator_create(void)
{
	jitter_estimator_t *je = calloc(1, sizeof(jitter_estimator_t));
	if (je) {
		jitter_estimator_reset(je);
	}
	return je;
}

void jitter_estimator_destroy(jitter_estimator_t *je)
{
	free(je);
}

void jitter_estimator_reset(jitter_estimator_t *je)
{
	if (!je)
		return;

	kalman_reset(&je->kalman);

	je->avg_frame_size = 500.0;
	je->max_frame_size = 500.0;
	je->var_frame_size = 100.0;

	je->avg_noise_ms = 0.0;
	je->var_noise_ms2 = 4.0;

	je->startup_count = 0;
	je->alpha_count = 1;
	je->prev_frame_size = 0.0;
	je->has_prev_frame = false;
	je->prev_estimate = MIN_JITTER_MS;
	je->filtered_estimate = 0.0;

	memset(je->frame_times, 0, sizeof(je->frame_times));
	je->fps_index = 0;
	je->fps_count = 0;
	je->last_update_time_us = 0;

	// Reset IFDV tracking
	je->prev_rtp_timestamp = 0;
	je->prev_receive_time_us = 0;
	je->has_prev_rtp = false;

	// Reset CUSUM detection
	je->cusum_pos = 0.0;
	je->cusum_neg = 0.0;
	je->cusum_alarm_count = 0;

	// Reset max-gap tracking
	memset(je->gap_history, 0, sizeof(je->gap_history));
	je->gap_index = 0;
	je->gap_count = 0;
	je->max_gap_ms = 0.0;
	je->smoothed_max_gap_ms = 100.0; // Start conservative
	je->last_buffer_target = 8;      // Start with reasonable default (~250ms at 30fps)
}

void jitter_estimator_update(jitter_estimator_t *je, double frame_delay_ms, size_t frame_size)
{
	if (!je || frame_size == 0)
		return;

	double frame_size_d = (double)frame_size;

	// Update FPS tracking
	uint64_t now_us = get_time_us();
	if (je->last_update_time_us > 0) {
		double delta_ms = (double)(now_us - je->last_update_time_us) / 1000.0;
		je->frame_times[je->fps_index] = delta_ms;
		je->fps_index = (je->fps_index + 1) % FPS_WINDOW;
		if (je->fps_count < FPS_WINDOW)
			je->fps_count++;
	}
	je->last_update_time_us = now_us;

	// Update frame size statistics
	double delta_frame = frame_size_d - je->prev_frame_size;

	// IIR filter for average frame size (skip keyframes)
	double deviation_size = 2.0 * sqrt(je->var_frame_size);
	if (frame_size_d < je->avg_frame_size + deviation_size) {
		je->avg_frame_size = PHI * je->avg_frame_size + (1.0 - PHI) * frame_size_d;
	}

	// Update variance
	double delta_from_avg = frame_size_d - je->avg_frame_size;
	je->var_frame_size = fmax(PHI * je->var_frame_size + (1.0 - PHI) * delta_from_avg * delta_from_avg, 1.0);

	// Update max frame size (non-linear IIR)
	je->max_frame_size = fmax(PSI * je->max_frame_size, frame_size_d);

	if (!je->has_prev_frame) {
		je->prev_frame_size = frame_size_d;
		je->has_prev_frame = true;
		return;
	}
	je->prev_frame_size = frame_size_d;

	// Clamp frame delay based on current noise estimate
	double max_deviation = NUM_STDDEV_DELAY_CLAMP * sqrt(je->var_noise_ms2);
	frame_delay_ms = fmax(fmin(frame_delay_ms, max_deviation), -max_deviation);

	// Calculate deviation from Kalman prediction
	double predicted = kalman_predict(&je->kalman, delta_frame);
	double delay_deviation = frame_delay_ms - predicted;

	// Outlier detection
	bool is_delay_outlier = fabs(delay_deviation) >= NUM_STDDEV_DELAY_OUTLIER * sqrt(je->var_noise_ms2);
	bool is_size_outlier = frame_size_d > je->avg_frame_size + NUM_STDDEV_SIZE_OUTLIER * sqrt(je->var_frame_size);

	// Congestion detection
	bool is_congested = delta_frame <= CONGESTION_REJECTION_FACTOR * je->max_frame_size;

	// Update noise estimate
	if (!is_delay_outlier || is_size_outlier) {
		// FPS-based alpha scaling
		double fps = jitter_estimator_get_fps(je);
		double alpha = (double)(je->alpha_count - 1) / (double)je->alpha_count;

		if (fps > 0) {
			double rate_scale = 30.0 / fps;
			if (je->alpha_count < STARTUP_COUNT) {
				rate_scale = (je->alpha_count * rate_scale + (STARTUP_COUNT - je->alpha_count)) /
					     STARTUP_COUNT;
			}
			alpha = pow(alpha, rate_scale);
		}

		je->alpha_count++;
		if (je->alpha_count > ALPHA_COUNT_MAX) {
			je->alpha_count = ALPHA_COUNT_MAX;
		}

		double noise_sample = is_delay_outlier ? (delay_deviation >= 0 ? NUM_STDDEV_DELAY_OUTLIER
									       : -NUM_STDDEV_DELAY_OUTLIER) *
								 sqrt(je->var_noise_ms2)
						       : delay_deviation;

		je->avg_noise_ms = alpha * je->avg_noise_ms + (1 - alpha) * noise_sample;
		je->var_noise_ms2 = alpha * je->var_noise_ms2 +
				    (1 - alpha) * (noise_sample - je->avg_noise_ms) * (noise_sample - je->avg_noise_ms);
		je->var_noise_ms2 = fmax(je->var_noise_ms2, 1.0);
	}

	// Update Kalman filter (skip outliers and congested frames)
	if ((!is_delay_outlier || is_size_outlier) && !is_congested) {
		kalman_update(&je->kalman, frame_delay_ms, delta_frame, je->max_frame_size, je->var_noise_ms2);
	}

	// Post-process after startup
	if (je->startup_count >= STARTUP_COUNT) {
		je->filtered_estimate = jitter_estimator_get_ms(je);
	} else {
		je->startup_count++;
	}
}

double jitter_estimator_get_ms(jitter_estimator_t *je)
{
	if (!je)
		return MIN_JITTER_MS;

	// Calculate jitter from worst-case frame size deviation
	double worst_case_delta = je->max_frame_size - je->avg_frame_size;
	double jitter_ms = je->kalman.slope * worst_case_delta;

	// Add noise threshold
	double noise_threshold = NOISE_STDDEVS * sqrt(je->var_noise_ms2) - NOISE_STDDEV_OFFSET;
	if (noise_threshold < 1.0)
		noise_threshold = 1.0;
	jitter_ms += noise_threshold;

	// Clamp
	if (jitter_ms < MIN_JITTER_MS) {
		jitter_ms = je->prev_estimate;
	}
	jitter_ms = fmax(MIN_JITTER_MS, fmin(jitter_ms, MAX_JITTER_MS));

	je->prev_estimate = jitter_ms;

	// Add OS jitter
	jitter_ms += OS_JITTER_MS;

	// Use filtered estimate if available
	if (je->filtered_estimate > jitter_ms) {
		jitter_ms = je->filtered_estimate;
	}

	// Scale by frame rate (ignore jitter for very low fps)
	double fps = jitter_estimator_get_fps(je);
	if (fps > 0 && fps < 5.0) {
		return 0.0; // Ignore jitter for < 5fps
	} else if (fps >= 5.0 && fps < 10.0) {
		// Linear interpolation from 0 at 5fps to full at 10fps
		jitter_ms *= (fps - 5.0) / 5.0;
	}

	return fmax(0.0, jitter_ms);
}

int jitter_estimator_get_buffer_target(jitter_estimator_t *je, double fps)
{
	if (!je)
		return MIN_BUFFER_TARGET;

	// Sanity check: need valid FPS and gap data
	if (fps < 5.0 || fps > 120.0 || je->gap_count < 5) {
		return je->last_buffer_target; // Not enough data yet
	}

	// Sanity check: smoothed_max_gap should be reasonable (1ms to 2000ms)
	double max_gap = je->smoothed_max_gap_ms;
	if (max_gap < 1.0 || max_gap > 2000.0) {
		return je->last_buffer_target; // Invalid data
	}

	// For bursty AI video, use max-gap based approach
	double frame_duration_ms = 1000.0 / fps;

	// Calculate target: enough buffer to survive the longest gap
	int target = (int)ceil(max_gap / frame_duration_ms) + 2; // +2 safety margin

	// Clamp to reasonable range
	if (target < MIN_BUFFER_TARGET)
		target = MIN_BUFFER_TARGET;
	if (target > MAX_BUFFER_TARGET)
		target = MAX_BUFFER_TARGET;

	// Apply hysteresis: only change if difference is significant
	int diff = target - je->last_buffer_target;
	if (diff > 0 && diff < BUFFER_TARGET_HYSTERESIS) {
		return je->last_buffer_target; // Don't increase for small changes
	}
	if (diff < 0 && diff > -BUFFER_TARGET_HYSTERESIS) {
		return je->last_buffer_target; // Don't decrease for small changes
	}

	// Log target changes
	if (target != je->last_buffer_target) {
		blog(LOG_INFO, "[Buffer] target %d->%d (max_gap=%.0fms, fps=%.1f)", je->last_buffer_target, target,
		     max_gap, fps);
	}

	je->last_buffer_target = target;
	return target;
}

double jitter_estimator_get_fps(jitter_estimator_t *je)
{
	if (!je || je->fps_count == 0)
		return 0.0;

	double sum = 0.0;
	for (int i = 0; i < je->fps_count; i++) {
		sum += je->frame_times[i];
	}
	double avg_ms = sum / je->fps_count;

	if (avg_ms <= 0)
		return 0.0;
	return 1000.0 / avg_ms;
}

// CUSUM detection for sudden delay changes (from Chrome WebRTC)
// Returns true if a significant delay change is detected
static bool cusum_delay_change_detection(jitter_estimator_t *je, double residual_ms)
{
	// Convert to RTP ticks for comparison with Chrome constants
	double residual_ticks = residual_ms * 90.0;

	// Clamp residual
	double clamped = residual_ticks;
	if (clamped > CUSUM_ACC_MAX_ERROR)
		clamped = CUSUM_ACC_MAX_ERROR;
	if (clamped < -CUSUM_ACC_MAX_ERROR)
		clamped = -CUSUM_ACC_MAX_ERROR;

	// Update CUSUM accumulators
	je->cusum_pos = fmax(je->cusum_pos + clamped - CUSUM_ACC_DRIFT, 0.0);
	je->cusum_neg = fmin(je->cusum_neg + clamped + CUSUM_ACC_DRIFT, 0.0);

	// Check for alarm
	if (je->cusum_pos > CUSUM_ALARM_THRESHOLD || je->cusum_neg < -CUSUM_ALARM_THRESHOLD) {
		// Reset accumulators
		double old_pos = je->cusum_pos;
		double old_neg = je->cusum_neg;
		je->cusum_pos = 0.0;
		je->cusum_neg = 0.0;
		je->cusum_alarm_count++;
		blog(LOG_INFO, "[CUSUM] Delay change detected #%d (pos=%.0f, neg=%.0f, residual=%.1fms)",
		     je->cusum_alarm_count, old_pos, old_neg, residual_ms);
		return true;
	}

	return false;
}

void jitter_estimator_update_rtp(jitter_estimator_t *je, uint32_t rtp_timestamp, uint64_t receive_time_us,
				 size_t frame_size)
{
	if (!je || frame_size == 0)
		return;

	// Update FPS and max-gap tracking using wall clock
	if (je->last_update_time_us > 0 && receive_time_us > je->last_update_time_us) {
		double delta_ms = (double)(receive_time_us - je->last_update_time_us) / 1000.0;

		// Sanity check: delta should be reasonable (0.1ms to 5000ms)
		if (delta_ms > 0.1 && delta_ms < 5000.0) {
			// FPS tracking
			je->frame_times[je->fps_index] = delta_ms;
			je->fps_index = (je->fps_index + 1) % FPS_WINDOW;
			if (je->fps_count < FPS_WINDOW)
				je->fps_count++;

			// Max-gap tracking (key for bursty AI video)
			je->gap_history[je->gap_index] = delta_ms;
			je->gap_index = (je->gap_index + 1) % MAX_GAP_WINDOW;
			if (je->gap_count < MAX_GAP_WINDOW)
				je->gap_count++;

			// Find max gap in window
			je->max_gap_ms = 0.0;
			for (int i = 0; i < je->gap_count; i++) {
				if (je->gap_history[i] > je->max_gap_ms)
					je->max_gap_ms = je->gap_history[i];
			}

			// Smooth the max gap with EMA (alpha=0.1 for stability)
			je->smoothed_max_gap_ms = 0.9 * je->smoothed_max_gap_ms + 0.1 * je->max_gap_ms;
		}
	}
	je->last_update_time_us = receive_time_us;

	// Calculate Inter-Frame Delay Variation (IFDV)
	// IFDV = (wall_clock_delta) - (rtp_timestamp_delta / 90000)
	// This is the key difference from simple wall clock delay
	double frame_delay_ms = 0.0;

	if (je->has_prev_rtp) {
		// Wall clock delta in ms
		double wall_delta_ms = (double)(receive_time_us - je->prev_receive_time_us) / 1000.0;

		// RTP timestamp delta (handle wraparound)
		int32_t rtp_delta = (int32_t)(rtp_timestamp - je->prev_rtp_timestamp);
		double rtp_delta_ms = (double)rtp_delta / 90.0; // 90kHz clock

		// IFDV: how much later/earlier than expected
		frame_delay_ms = wall_delta_ms - rtp_delta_ms;
	}

	je->prev_rtp_timestamp = rtp_timestamp;
	je->prev_receive_time_us = receive_time_us;
	je->has_prev_rtp = true;

	// Log IFDV periodically for debugging (every 30 frames)
	static int ifdv_log_counter = 0;
	if (je->has_prev_rtp && ifdv_log_counter++ % 30 == 0) {
		blog(LOG_INFO, "[IFDV] delay_variation=%.1fms", frame_delay_ms);
	}

	// CUSUM detection for persistent delay changes
	bool delay_change_detected = cusum_delay_change_detection(je, frame_delay_ms);

	if (delay_change_detected) {
		// Persistent delay change detected - soft reset the Kalman filter
		// This allows faster adaptation to new network conditions
		je->kalman.intercept_var = 1e10; // Increase uncertainty
		je->alpha_count = 1;             // Reset noise filter
		blog(LOG_INFO, "[IFDV] Kalman filter reset due to CUSUM alarm");
	}

	// Now call the regular update with the computed IFDV
	jitter_estimator_update(je, frame_delay_ms, frame_size);
}
