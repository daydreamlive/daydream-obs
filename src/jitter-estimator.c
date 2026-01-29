/*
 * Jitter Estimator - Based on Chrome WebRTC implementation
 * Simplified Kalman filter approach for OBS plugin
 */

#include "jitter-estimator.h"
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
	if (!je || fps <= 0)
		return 10; // Default

	double jitter_ms = jitter_estimator_get_ms(je);

	// Convert jitter in ms to frames
	double frame_duration_ms = 1000.0 / fps;
	int target = (int)ceil(jitter_ms / frame_duration_ms);

	// Clamp to reasonable range
	if (target < 2)
		target = 2;
	if (target > 60)
		target = 60;

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
