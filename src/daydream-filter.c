#include "daydream-filter.h"
#include "daydream-api.h"
#include "daydream-auth.h"
#include "daydream-encoder.h"
#include "daydream-decoder.h"
#include "daydream-whip.h"
#include "daydream-whep.h"
#include "jitter-estimator.h"
#include "timestamp-extrapolator.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#define PROP_LOGIN "login"
#define PROP_LOGOUT "logout"
#define PROP_LOGIN_STATUS "login_status"
#define PROP_PROMPT "prompt"
#define PROP_NEGATIVE_PROMPT "negative_prompt"
#define PROP_MODEL "model"
#define PROP_GUIDANCE "guidance"
#define PROP_DELTA "delta"
#define PROP_STEPS "steps"
#define PROP_START "start"
#define PROP_STOP "stop"
#define PROP_SMOOTH_MODE "smooth_mode"
#define PROP_BUFFER_TARGET "buffer_target"
#define PROP_ADAPT_SPEED "adapt_speed"
#define PROP_SPEED_MIN "speed_min"
#define PROP_SPEED_MAX "speed_max"

#define RAW_QUEUE_SIZE 32

// Jitter buffer for smooth playback
#define JITTER_BUFFER_SIZE 60          // ~2 seconds @ 30fps
#define JITTER_DELAY_MS 500            // 500ms buffering delay
#define RTP_CLOCK_RATE 90000           // H264 RTP clock rate

struct jitter_frame {
	uint8_t *data;
	size_t size;
	uint32_t width;
	uint32_t height;
	uint32_t rtp_timestamp;
	uint64_t receive_time_ns;
};

struct raw_packet {
	uint8_t *data;
	size_t size;
	uint32_t rtp_timestamp;
	uint64_t receive_time_ns;
};

struct daydream_filter {
	obs_source_t *source;

	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	gs_texture_t *output_texture;
	uint32_t width;
	uint32_t height;

	gs_texrender_t *crop_texrender;
	gs_stagesurf_t *crop_stagesurface;

	struct daydream_auth *auth;

	char *prompt;
	char *negative_prompt;
	char *model;
	float guidance;
	float delta;
	int steps;

	char *stream_id;
	char *whip_url;
	char *whep_url;
	bool streaming;
	bool stopping;

	struct daydream_encoder *encoder;
	struct daydream_decoder *decoder;
	struct daydream_whip *whip;
	struct daydream_whep *whep;

	pthread_t encode_thread;
	bool encode_thread_running;

	pthread_t decode_thread;
	bool decode_thread_running;

	pthread_t whep_thread;
	bool whep_thread_running;

	pthread_t start_thread;
	bool start_thread_running;

	struct raw_packet raw_queue[RAW_QUEUE_SIZE];
	int raw_queue_head;
	int raw_queue_tail;
	int raw_queue_count;
	pthread_mutex_t raw_mutex;
	pthread_cond_t raw_cond;

	uint8_t *pending_frame;
	uint32_t pending_frame_width;
	uint32_t pending_frame_height;
	uint32_t pending_frame_linesize;
	bool pending_frame_ready;

	uint8_t *decoded_frame;
	uint32_t decoded_frame_width;
	uint32_t decoded_frame_height;
	bool decoded_frame_ready;

	pthread_mutex_t mutex;
	pthread_cond_t frame_cond;

	uint64_t frame_count;
	uint64_t last_encode_time;
	uint32_t target_fps;

	// Jitter buffer for smooth playback (sorted by RTP timestamp)
	bool smooth_mode;
	struct jitter_frame jitter_buffer[JITTER_BUFFER_SIZE];
	int jitter_count; // Number of frames in buffer (index 0 = oldest)
	uint64_t jitter_playback_start_time;
	uint32_t jitter_playback_start_rtp;
	bool jitter_playback_started;
	pthread_mutex_t jitter_mutex;

	// Adaptive playback rate control
	int buffer_target;   // Target buffer level (frames)
	float adapt_speed;   // How fast to adapt (0.01-0.5)
	float speed_min;     // Minimum playback speed
	float speed_max;     // Maximum playback speed
	float current_speed; // Smoothed current speed
	uint64_t last_render_time;
	double accumulated_rtp; // Use double for precision

	// Chrome-style jitter estimator and timestamp extrapolator
	jitter_estimator_t *jitter_est;
	timestamp_extrapolator_t *ts_extrap;
	uint64_t last_frame_time_us; // For frame delay calculation
	double current_delay_ms;     // Current playout delay (jitter buffer delay)

	// FPS measurement
	uint64_t fps_measure_start;
	int fps_frame_count;

	// Frame interpolation
	uint8_t *interp_frame_a;   // Previous frame for interpolation
	uint8_t *interp_frame_b;   // Next frame for interpolation
	uint8_t *interp_frame_out; // Blended output frame
	uint32_t interp_width;
	uint32_t interp_height;
	uint32_t interp_rtp_a;     // PTS of frame A
	uint32_t interp_rtp_b;     // PTS of frame B
	uint64_t interp_receive_a; // Receive time of frame A (for latency tracking)
	uint64_t interp_receive_b; // Receive time of frame B
	bool interp_has_a;
	bool interp_has_b;
};

static const char *daydream_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Daydream";
}

static void daydream_filter_update(void *data, obs_data_t *settings)
{
	struct daydream_filter *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	bfree(ctx->prompt);
	bfree(ctx->negative_prompt);
	bfree(ctx->model);

	ctx->prompt = bstrdup(obs_data_get_string(settings, PROP_PROMPT));
	ctx->negative_prompt = bstrdup(obs_data_get_string(settings, PROP_NEGATIVE_PROMPT));
	ctx->model = bstrdup(obs_data_get_string(settings, PROP_MODEL));
	ctx->guidance = (float)obs_data_get_double(settings, PROP_GUIDANCE);
	ctx->delta = (float)obs_data_get_double(settings, PROP_DELTA);
	ctx->steps = (int)obs_data_get_int(settings, PROP_STEPS);
	ctx->smooth_mode = obs_data_get_bool(settings, PROP_SMOOTH_MODE);

	// Adaptive rate control parameters
	ctx->buffer_target = (int)obs_data_get_int(settings, PROP_BUFFER_TARGET);
	ctx->adapt_speed = (float)obs_data_get_double(settings, PROP_ADAPT_SPEED);
	ctx->speed_min = (float)obs_data_get_double(settings, PROP_SPEED_MIN);
	ctx->speed_max = (float)obs_data_get_double(settings, PROP_SPEED_MAX);

	pthread_mutex_unlock(&ctx->mutex);
}

static void on_whep_frame(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(is_keyframe);
	uint64_t receive_time_ns = os_gettime_ns();

	if (!ctx)
		return;

	// === DEBUG: Hypothesis 1,2 - frame receive interval and keyframe detection ===
	static uint64_t last_whep_frame_time = 0;
	static uint64_t last_keyframe_time = 0;
	static uint64_t whep_frame_count = 0;
	uint64_t now = os_gettime_ns();

	// H264 keyframe detection (NAL unit type 5 = IDR, type 7 = SPS)
	bool detected_keyframe = false;
	if (size > 4) {
		// Check for start code (0x00000001 or 0x000001)
		const uint8_t *p = data;
		if ((p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) || (p[0] == 0 && p[1] == 0 && p[2] == 1)) {
			int offset = (p[2] == 1) ? 3 : 4;
			uint8_t nal_type = p[offset] & 0x1F;
			detected_keyframe = (nal_type == 5 || nal_type == 7);
		}
	}

	uint64_t gap_ms = (last_whep_frame_time > 0) ? (now - last_whep_frame_time) / 1000000 : 0;

	// Log if gap > 100ms, otherwise every 30 frames
	if (gap_ms > 100 || whep_frame_count % 30 == 0) {
		blog(LOG_INFO, "[DEBUG WHEP] frame #%llu, gap=%llums, size=%zu, keyframe=%s",
		     (unsigned long long)whep_frame_count, (unsigned long long)gap_ms, size,
		     detected_keyframe ? "YES" : "no");
	}

	if (detected_keyframe) {
		uint64_t keyframe_gap_ms = (last_keyframe_time > 0) ? (now - last_keyframe_time) / 1000000 : 0;
		blog(LOG_INFO, "[DEBUG WHEP] === KEYFRAME === gap since last keyframe: %llums",
		     (unsigned long long)keyframe_gap_ms);
		last_keyframe_time = now;
	}

	last_whep_frame_time = now;
	whep_frame_count++;
	// === END DEBUG ===

	pthread_mutex_lock(&ctx->raw_mutex);

	// === DEBUG: Hypothesis 5 - queue overflow ===
	if (ctx->raw_queue_count >= RAW_QUEUE_SIZE) {
		blog(LOG_WARNING, "[DEBUG WHEP] Queue overflow! Dropping oldest frame. queue_count=%d",
		     ctx->raw_queue_count);
		ctx->raw_queue_tail = (ctx->raw_queue_tail + 1) % RAW_QUEUE_SIZE;
		ctx->raw_queue_count--;
	}

	struct raw_packet *pkt = &ctx->raw_queue[ctx->raw_queue_head];

	if (!pkt->data || pkt->size < size) {
		bfree(pkt->data);
		pkt->data = bmalloc(size);
	}

	memcpy(pkt->data, data, size);
	pkt->size = size;
	pkt->rtp_timestamp = timestamp;
	pkt->receive_time_ns = receive_time_ns;

	ctx->raw_queue_head = (ctx->raw_queue_head + 1) % RAW_QUEUE_SIZE;
	ctx->raw_queue_count++;

	pthread_cond_signal(&ctx->raw_cond);
	pthread_mutex_unlock(&ctx->raw_mutex);
}

static void *decode_thread_func(void *data)
{
	struct daydream_filter *ctx = data;

	blog(LOG_INFO, "[Daydream Decode] Thread started");

	while (ctx->decode_thread_running) {
		pthread_mutex_lock(&ctx->raw_mutex);

		while (ctx->raw_queue_count == 0 && ctx->decode_thread_running && !ctx->stopping) {
			pthread_cond_wait(&ctx->raw_cond, &ctx->raw_mutex);
		}

		if (!ctx->decode_thread_running || ctx->stopping) {
			pthread_mutex_unlock(&ctx->raw_mutex);
			break;
		}

		struct raw_packet *pkt = &ctx->raw_queue[ctx->raw_queue_tail];
		uint8_t *raw_data = bmalloc(pkt->size);
		size_t raw_size = pkt->size;
		uint32_t rtp_timestamp = pkt->rtp_timestamp;
		uint64_t receive_time_ns = pkt->receive_time_ns;
		memcpy(raw_data, pkt->data, raw_size);

		// Measure queue wait time
		uint64_t queue_wait_ms = (os_gettime_ns() - receive_time_ns) / 1000000;
		if (queue_wait_ms > 50) {
			blog(LOG_INFO, "[Queue] Wait time: %llums, queue_depth=%d", (unsigned long long)queue_wait_ms,
			     ctx->raw_queue_count);
		}

		ctx->raw_queue_tail = (ctx->raw_queue_tail + 1) % RAW_QUEUE_SIZE;
		ctx->raw_queue_count--;

		pthread_mutex_unlock(&ctx->raw_mutex);

		// === DEBUG: Hypothesis 3 - decode interval and success rate ===
		static uint64_t last_decode_time = 0;
		static uint64_t decode_success_count = 0;
		static uint64_t decode_fail_count = 0;

		uint64_t decode_start = os_gettime_ns();
		uint64_t decode_gap_ms = (last_decode_time > 0) ? (decode_start - last_decode_time) / 1000000 : 0;

		struct daydream_decoded_frame decoded;
		bool decode_success = ctx->decoder &&
				      daydream_decoder_decode(ctx->decoder, raw_data, raw_size, &decoded);

		if (decode_success) {
			decode_success_count++;
			uint64_t decode_time = (os_gettime_ns() - decode_start) / 1000000;

			// Log if gap > 100ms or every 30 frames
			if (decode_gap_ms > 100 || decode_success_count % 30 == 0) {
				blog(LOG_INFO, "[DEBUG Decode] #%llu, gap=%llums, decode_time=%llums, size=%zu",
				     (unsigned long long)decode_success_count, (unsigned long long)decode_gap_ms,
				     (unsigned long long)decode_time, raw_size);
			}

			if (decode_time > 15) {
				blog(LOG_WARNING, "[DEBUG Decode] Slow decode: %llums, size=%zu",
				     (unsigned long long)decode_time, raw_size);
			}

			last_decode_time = os_gettime_ns();

			// Measure actual FPS from server
			uint64_t now_ns = os_gettime_ns();
			if (ctx->fps_measure_start == 0) {
				ctx->fps_measure_start = now_ns;
				ctx->fps_frame_count = 0;
			}
			ctx->fps_frame_count++;
			uint64_t elapsed_ms = (now_ns - ctx->fps_measure_start) / 1000000;
			if (elapsed_ms >= 5000) { // Report every 5 seconds
				double actual_fps = (double)ctx->fps_frame_count * 1000.0 / (double)elapsed_ms;
				blog(LOG_INFO, "[FPS] Server output: %.1f fps (%d frames in %llums)", actual_fps,
				     ctx->fps_frame_count, (unsigned long long)elapsed_ms);
				ctx->fps_measure_start = now_ns;
				ctx->fps_frame_count = 0;
			}

			size_t frame_size = decoded.linesize * decoded.height;

			if (ctx->smooth_mode) {
				// Smooth mode: add to jitter buffer
				pthread_mutex_lock(&ctx->jitter_mutex);

				// Update timestamp extrapolator and jitter estimator
				uint64_t now_us = receive_time_ns / 1000;

				// Update timestamp extrapolator (RLS filter for RTP->local time mapping)
				if (ctx->ts_extrap) {
					timestamp_extrapolator_update(ctx->ts_extrap, now_us, rtp_timestamp);
				}

				// Update jitter estimator with RTP timestamp (more accurate IFDV calculation)
				if (ctx->jitter_est) {
					// Use RTP-based update for proper inter-frame delay variation
					// This accounts for actual frame timing from the source
					jitter_estimator_update_rtp(ctx->jitter_est, rtp_timestamp, now_us, raw_size);

					// Sync buffer target from jitter estimator (may increase on underruns)
					int new_target = jitter_estimator_get_buffer_target(ctx->jitter_est, 0);
					if (new_target != ctx->buffer_target) {
						ctx->buffer_target = new_target;
					}
				}

				if (ctx->jitter_count >= JITTER_BUFFER_SIZE) {
					// Buffer full, drop oldest (index 0)
					bfree(ctx->jitter_buffer[0].data);
					ctx->jitter_buffer[0].data = NULL;
					// Shift all frames down
					for (int i = 0; i < ctx->jitter_count - 1; i++) {
						ctx->jitter_buffer[i] = ctx->jitter_buffer[i + 1];
					}
					ctx->jitter_buffer[ctx->jitter_count - 1].data = NULL;
					ctx->jitter_count--;
				}

				// Find sorted insertion position by RTP timestamp
				// Use signed comparison to handle RTP wraparound
				int insert_pos = ctx->jitter_count; // Default: append at end
				for (int i = ctx->jitter_count - 1; i >= 0; i--) {
					int32_t diff = (int32_t)(rtp_timestamp - ctx->jitter_buffer[i].rtp_timestamp);
					if (diff > 0) {
						// New frame is after frame[i], insert after it
						insert_pos = i + 1;
						break;
					} else if (diff == 0) {
						// Duplicate timestamp, skip
						blog(LOG_WARNING, "[Jitter] Duplicate RTP timestamp %u, skipping",
						     rtp_timestamp);
						pthread_mutex_unlock(&ctx->jitter_mutex);
						goto skip_jitter_insert;
					}
					insert_pos = i; // New frame is before frame[i]
				}

				// Log out-of-order insertions
				bool out_of_order = (insert_pos < ctx->jitter_count);
				if (out_of_order) {
					blog(LOG_INFO, "[Jitter] Out-of-order frame: rtp=%u inserted at pos %d/%d",
					     rtp_timestamp, insert_pos, ctx->jitter_count);
				}

				// Shift frames to make room for insertion
				if (insert_pos < ctx->jitter_count) {
					for (int i = ctx->jitter_count; i > insert_pos; i--) {
						ctx->jitter_buffer[i] = ctx->jitter_buffer[i - 1];
					}
					ctx->jitter_buffer[insert_pos].data = NULL; // Clear moved pointer
				}

				// Insert the new frame
				struct jitter_frame *jf = &ctx->jitter_buffer[insert_pos];
				if (!jf->data || jf->size < frame_size) {
					bfree(jf->data);
					jf->data = bmalloc(frame_size);
				}
				memcpy(jf->data, decoded.data, frame_size);
				jf->size = frame_size;
				jf->width = decoded.width;
				jf->height = decoded.height;
				jf->rtp_timestamp = rtp_timestamp;
				jf->receive_time_ns = receive_time_ns;

				ctx->jitter_count++;

				// Log buffer state periodically
				static int jitter_log_counter = 0;
				if (jitter_log_counter++ % 30 == 0) {
					uint32_t oldest_rtp = ctx->jitter_buffer[0].rtp_timestamp;
					uint32_t newest_rtp = ctx->jitter_buffer[ctx->jitter_count - 1].rtp_timestamp;
					int32_t rtp_span = (int32_t)(newest_rtp - oldest_rtp);
					double span_ms = (double)rtp_span / 90.0;
					blog(LOG_INFO,
					     "[Jitter] Buffer: count=%d, rtp_span=%.1fms (oldest=%u, newest=%u)",
					     ctx->jitter_count, span_ms, oldest_rtp, newest_rtp);
				}

				pthread_mutex_unlock(&ctx->jitter_mutex);
			} else if (0) {
			skip_jitter_insert:; // Label requires a statement
			} else {
				// Normal mode: directly copy to decoded_frame
				pthread_mutex_lock(&ctx->mutex);

				if (!ctx->decoded_frame || ctx->decoded_frame_width != decoded.width ||
				    ctx->decoded_frame_height != decoded.height) {
					bfree(ctx->decoded_frame);
					ctx->decoded_frame = bmalloc(frame_size);
				}

				memcpy(ctx->decoded_frame, decoded.data, frame_size);
				ctx->decoded_frame_width = decoded.width;
				ctx->decoded_frame_height = decoded.height;
				ctx->decoded_frame_ready = true;

				pthread_mutex_unlock(&ctx->mutex);
			}
		} else {
			// === DEBUG: Decode failed ===
			decode_fail_count++;
			blog(LOG_WARNING, "[DEBUG Decode] FAILED! total_fails=%llu, size=%zu",
			     (unsigned long long)decode_fail_count, raw_size);
		}

		bfree(raw_data);
	}

	blog(LOG_INFO, "[Daydream Decode] Thread stopped");
	return NULL;
}

static void on_whep_state(bool connected, const char *error, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(ctx);

	if (connected) {
		blog(LOG_INFO, "[Daydream] WHEP connected");
	} else {
		blog(LOG_INFO, "[Daydream] WHEP disconnected: %s", error ? error : "");
	}
}

static void on_whip_state(bool connected, const char *error, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(ctx);

	if (connected) {
		blog(LOG_INFO, "[Daydream] WHIP connected");
	} else {
		blog(LOG_INFO, "[Daydream] WHIP disconnected: %s", error ? error : "");
	}
}

static void *whep_connect_thread_func(void *data)
{
	struct daydream_filter *ctx = data;

	blog(LOG_INFO, "[Daydream] WHEP connect thread started");

	if (daydream_whep_connect(ctx->whep)) {
		blog(LOG_INFO, "[Daydream] WHEP connected successfully");
	} else {
		blog(LOG_ERROR, "[Daydream] WHEP connection failed");
	}

	ctx->whep_thread_running = false;
	return NULL;
}

static void *encode_thread_func(void *data)
{
	struct daydream_filter *ctx = data;

	uint64_t frame_interval_ns = 1000000000ULL / ctx->target_fps;

	while (ctx->encode_thread_running) {
		pthread_mutex_lock(&ctx->mutex);

		while (!ctx->pending_frame_ready && ctx->encode_thread_running && !ctx->stopping) {
			pthread_cond_wait(&ctx->frame_cond, &ctx->mutex);
		}

		if (!ctx->encode_thread_running || ctx->stopping) {
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}

		if (!ctx->pending_frame_ready) {
			pthread_mutex_unlock(&ctx->mutex);
			continue;
		}

		uint8_t *frame_copy = bmalloc(ctx->pending_frame_linesize * ctx->pending_frame_height);
		memcpy(frame_copy, ctx->pending_frame, ctx->pending_frame_linesize * ctx->pending_frame_height);
		uint32_t frame_linesize = ctx->pending_frame_linesize;
		ctx->pending_frame_ready = false;

		pthread_mutex_unlock(&ctx->mutex);

		bool has_encoder = ctx->encoder != NULL;
		bool has_whip = ctx->whip != NULL;
		bool whip_connected = has_whip && daydream_whip_is_connected(ctx->whip);

		if (has_encoder && whip_connected) {
			struct daydream_encoded_frame encoded;
			if (daydream_encoder_encode(ctx->encoder, frame_copy, frame_linesize, &encoded)) {
				uint32_t timestamp_ms = (uint32_t)(ctx->frame_count * 1000 / ctx->target_fps);
				daydream_whip_send_frame(ctx->whip, encoded.data, encoded.size, timestamp_ms,
							 encoded.is_keyframe);
				ctx->frame_count++;

				// Debug: log WHIP send rate
				static uint64_t last_whip_send_time = 0;
				static uint64_t whip_send_count = 0;
				uint64_t now_send = os_gettime_ns();
				uint64_t send_gap_ms =
					(last_whip_send_time > 0) ? (now_send - last_whip_send_time) / 1000000 : 0;
				whip_send_count++;

				if (send_gap_ms > 50 || whip_send_count % 30 == 0) {
					blog(LOG_INFO, "[DEBUG WHIP Send] #%llu, gap=%llums, size=%zu, keyframe=%s",
					     (unsigned long long)whip_send_count, (unsigned long long)send_gap_ms,
					     encoded.size, encoded.is_keyframe ? "YES" : "no");
				}
				last_whip_send_time = now_send;
			}
		}

		bfree(frame_copy);

		uint64_t now = os_gettime_ns();
		uint64_t elapsed = now - ctx->last_encode_time;
		if (elapsed < frame_interval_ns) {
			os_sleepto_ns(ctx->last_encode_time + frame_interval_ns);
		}
		ctx->last_encode_time = os_gettime_ns();
	}

	return NULL;
}

static void *daydream_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct daydream_filter *ctx = bzalloc(sizeof(struct daydream_filter));
	ctx->source = source;
	ctx->streaming = false;
	ctx->stopping = false;
	ctx->target_fps = 30;
	ctx->frame_count = 0;

	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->frame_cond, NULL);
	pthread_mutex_init(&ctx->raw_mutex, NULL);
	pthread_cond_init(&ctx->raw_cond, NULL);
	pthread_mutex_init(&ctx->jitter_mutex, NULL);

	// Initialize jitter buffer (sorted by RTP timestamp)
	ctx->jitter_count = 0;
	ctx->jitter_playback_started = false;
	ctx->current_speed = 1.0f;

	// Create Chrome-style jitter estimator and timestamp extrapolator
	ctx->jitter_est = jitter_estimator_create();
	ctx->ts_extrap = timestamp_extrapolator_create();
	ctx->last_frame_time_us = 0;
	ctx->current_delay_ms = 100.0; // Start with 100ms delay

	ctx->auth = daydream_auth_create();

	daydream_filter_update(ctx, settings);

	return ctx;
}

static void stop_streaming(struct daydream_filter *ctx)
{
	ctx->stopping = true;

	if (ctx->encode_thread_running) {
		ctx->encode_thread_running = false;
		pthread_cond_signal(&ctx->frame_cond);
		pthread_join(ctx->encode_thread, NULL);
	}

	if (ctx->decode_thread_running) {
		ctx->decode_thread_running = false;
		pthread_cond_signal(&ctx->raw_cond);
		pthread_join(ctx->decode_thread, NULL);
	}

	if (ctx->whep_thread_running) {
		pthread_join(ctx->whep_thread, NULL);
		ctx->whep_thread_running = false;
	}

	if (ctx->start_thread_running) {
		pthread_join(ctx->start_thread, NULL);
		ctx->start_thread_running = false;
	}

	if (ctx->whip) {
		daydream_whip_disconnect(ctx->whip);
		daydream_whip_destroy(ctx->whip);
		ctx->whip = NULL;
	}

	if (ctx->whep) {
		daydream_whep_disconnect(ctx->whep);
		daydream_whep_destroy(ctx->whep);
		ctx->whep = NULL;
	}

	if (ctx->encoder) {
		daydream_encoder_destroy(ctx->encoder);
		ctx->encoder = NULL;
	}

	if (ctx->decoder) {
		daydream_decoder_destroy(ctx->decoder);
		ctx->decoder = NULL;
	}

	ctx->streaming = false;
	ctx->stopping = false;
	ctx->decoded_frame_ready = false;

	ctx->raw_queue_head = 0;
	ctx->raw_queue_tail = 0;
	ctx->raw_queue_count = 0;

	// Reset jitter buffer
	ctx->jitter_count = 0;
	ctx->jitter_playback_started = false;

	// Reset adaptive playback
	ctx->last_render_time = 0;
	ctx->accumulated_rtp = 0;
	ctx->current_speed = 1.0f;

	// Reset jitter estimator and timestamp extrapolator
	if (ctx->jitter_est) {
		jitter_estimator_reset(ctx->jitter_est);
	}
	if (ctx->ts_extrap) {
		timestamp_extrapolator_reset(ctx->ts_extrap, os_gettime_ns() / 1000);
	}
	ctx->last_frame_time_us = 0;
	ctx->current_delay_ms = 100.0;

	// Reset FPS measurement
	ctx->fps_measure_start = 0;
	ctx->fps_frame_count = 0;

	// Reset interpolation state (keep buffers allocated for reuse)
	ctx->interp_has_a = false;
	ctx->interp_has_b = false;
}

static void daydream_filter_destroy(void *data)
{
	struct daydream_filter *ctx = data;

	stop_streaming(ctx);

	obs_enter_graphics();
	if (ctx->texrender)
		gs_texrender_destroy(ctx->texrender);
	if (ctx->stagesurface)
		gs_stagesurface_destroy(ctx->stagesurface);
	if (ctx->output_texture)
		gs_texture_destroy(ctx->output_texture);
	if (ctx->crop_texrender)
		gs_texrender_destroy(ctx->crop_texrender);
	if (ctx->crop_stagesurface)
		gs_stagesurface_destroy(ctx->crop_stagesurface);
	obs_leave_graphics();

	daydream_auth_destroy(ctx->auth);

	// Destroy jitter estimator and timestamp extrapolator
	jitter_estimator_destroy(ctx->jitter_est);
	timestamp_extrapolator_destroy(ctx->ts_extrap);

	bfree(ctx->prompt);
	bfree(ctx->negative_prompt);
	bfree(ctx->model);
	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);
	bfree(ctx->pending_frame);
	bfree(ctx->decoded_frame);

	// Free interpolation buffers
	bfree(ctx->interp_frame_a);
	bfree(ctx->interp_frame_b);
	bfree(ctx->interp_frame_out);

	for (int i = 0; i < RAW_QUEUE_SIZE; i++) {
		bfree(ctx->raw_queue[i].data);
	}

	for (int i = 0; i < JITTER_BUFFER_SIZE; i++) {
		bfree(ctx->jitter_buffer[i].data);
	}

	pthread_cond_destroy(&ctx->frame_cond);
	pthread_mutex_destroy(&ctx->mutex);
	pthread_cond_destroy(&ctx->raw_cond);
	pthread_mutex_destroy(&ctx->raw_mutex);
	pthread_mutex_destroy(&ctx->jitter_mutex);

	bfree(ctx);
}

static void daydream_filter_video_render(void *data, gs_effect_t *effect)
{
	struct daydream_filter *ctx = data;
	UNUSED_PARAMETER(effect);

	const uint32_t STREAM_SIZE = 512;

	obs_source_t *parent = obs_filter_get_parent(ctx->source);
	if (!parent)
		return;

	uint32_t parent_width = obs_source_get_base_width(parent);
	uint32_t parent_height = obs_source_get_base_height(parent);

	if (parent_width == 0 || parent_height == 0)
		return;

	if (ctx->width != parent_width || ctx->height != parent_height) {
		ctx->width = parent_width;
		ctx->height = parent_height;

		if (ctx->texrender) {
			gs_texrender_destroy(ctx->texrender);
			ctx->texrender = NULL;
		}
		if (ctx->stagesurface) {
			gs_stagesurface_destroy(ctx->stagesurface);
			ctx->stagesurface = NULL;
		}
		if (ctx->output_texture) {
			gs_texture_destroy(ctx->output_texture);
			ctx->output_texture = NULL;
		}
	}

	if (!ctx->texrender)
		ctx->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!ctx->stagesurface)
		ctx->stagesurface = gs_stagesurface_create(ctx->width, ctx->height, GS_BGRA);

	gs_texrender_reset(ctx->texrender);
	if (gs_texrender_begin(ctx->texrender, ctx->width, ctx->height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)ctx->width, 0.0f, (float)ctx->height, -100.0f, 100.0f);

		obs_source_video_render(parent);
		gs_texrender_end(ctx->texrender);
	}

	gs_texture_t *tex = gs_texrender_get_texture(ctx->texrender);
	if (!tex)
		return;

	if (ctx->streaming && ctx->encode_thread_running) {
		if (!ctx->crop_texrender)
			ctx->crop_texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
		if (!ctx->crop_stagesurface)
			ctx->crop_stagesurface = gs_stagesurface_create(STREAM_SIZE, STREAM_SIZE, GS_BGRA);

		float scale;
		if (parent_width < parent_height) {
			scale = (float)STREAM_SIZE / (float)parent_width;
		} else {
			scale = (float)STREAM_SIZE / (float)parent_height;
		}

		float scaled_width = parent_width * scale;
		float scaled_height = parent_height * scale;
		float offset_x = (scaled_width - STREAM_SIZE) / 2.0f;
		float offset_y = (scaled_height - STREAM_SIZE) / 2.0f;

		gs_texrender_reset(ctx->crop_texrender);
		if (gs_texrender_begin(ctx->crop_texrender, STREAM_SIZE, STREAM_SIZE)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

			gs_ortho(offset_x / scale, (offset_x + STREAM_SIZE) / scale, offset_y / scale,
				 (offset_y + STREAM_SIZE) / scale, -100.0f, 100.0f);

			gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_technique_t *tech = gs_effect_get_technique(default_effect, "Draw");
			gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), tex);

			gs_technique_begin(tech);
			gs_technique_begin_pass(tech, 0);
			gs_draw_sprite(tex, 0, ctx->width, ctx->height);
			gs_technique_end_pass(tech);
			gs_technique_end(tech);

			gs_texrender_end(ctx->crop_texrender);
		}

		gs_texture_t *crop_tex = gs_texrender_get_texture(ctx->crop_texrender);
		if (crop_tex) {
			gs_stage_texture(ctx->crop_stagesurface, crop_tex);

			uint8_t *video_data = NULL;
			uint32_t video_linesize = 0;

			if (gs_stagesurface_map(ctx->crop_stagesurface, &video_data, &video_linesize)) {
				pthread_mutex_lock(&ctx->mutex);

				size_t data_size = STREAM_SIZE * video_linesize;
				if (!ctx->pending_frame || ctx->pending_frame_width != STREAM_SIZE ||
				    ctx->pending_frame_height != STREAM_SIZE) {
					bfree(ctx->pending_frame);
					ctx->pending_frame = bmalloc(data_size);
					ctx->pending_frame_width = STREAM_SIZE;
					ctx->pending_frame_height = STREAM_SIZE;
					blog(LOG_INFO, "[Daydream Render] Frame buffer allocated: %ux%u (from %ux%u)",
					     STREAM_SIZE, STREAM_SIZE, parent_width, parent_height);
				}

				memcpy(ctx->pending_frame, video_data, data_size);
				ctx->pending_frame_linesize = video_linesize;
				ctx->pending_frame_ready = true;
				pthread_cond_signal(&ctx->frame_cond);

				pthread_mutex_unlock(&ctx->mutex);

				gs_stagesurface_unmap(ctx->crop_stagesurface);
			}
		}
	}

	gs_texture_t *output = tex;

	// === DEBUG: Hypothesis 4 - render frame update interval ===
	static uint64_t last_render_update_time = 0;
	static uint64_t render_update_count = 0;
	static uint64_t render_no_frame_count = 0;

	if (ctx->smooth_mode) {
		// Adaptive speed control optimized for bursty AI video
		// Key concepts from Chrome: gradual speed transitions
		pthread_mutex_lock(&ctx->jitter_mutex);

		uint64_t now = os_gettime_ns();

		// Start playback when we have minimum frames
		if (!ctx->jitter_playback_started) {
			int start_threshold = ctx->buffer_target / 2;
			if (start_threshold < 3)
				start_threshold = 3;

			if (ctx->jitter_count >= start_threshold) {
				struct jitter_frame *oldest = &ctx->jitter_buffer[0]; // Index 0 is always oldest
				ctx->jitter_playback_started = true;
				ctx->jitter_playback_start_time = now;
				ctx->jitter_playback_start_rtp = oldest->rtp_timestamp;
				ctx->current_speed = 1.0f;

				// Calculate initial latency: time since oldest frame was received
				uint64_t oldest_age_ms = (now - oldest->receive_time_ns) / 1000000;
				blog(LOG_INFO,
				     "[Smooth] Playback started with %d frames (target=%d), oldest_frame_age=%llums",
				     ctx->jitter_count, ctx->buffer_target, (unsigned long long)oldest_age_ms);
			}
		}

		if (ctx->jitter_playback_started) {
			// Calculate target speed based on buffer level
			float target_speed;

			if (ctx->jitter_count == 0) {
				// Emergency: buffer empty - notify for target increase
				jitter_estimator_notify_underrun(ctx->jitter_est);
				target_speed = ctx->speed_min;
			} else if (ctx->jitter_count < ctx->buffer_target) {
				// Below target: slow down proportionally
				float ratio = (float)ctx->jitter_count / (float)ctx->buffer_target;
				target_speed = ctx->speed_min + ratio * (1.0f - ctx->speed_min);
			} else {
				// At or above target: speed up to reduce latency
				float excess = (float)(ctx->jitter_count - ctx->buffer_target);
				float gain = 0.1f;
				target_speed = 1.0f + gain * excess;
				if (target_speed > ctx->speed_max)
					target_speed = ctx->speed_max;
			}

			// Chrome-inspired gradual speed change (prevents jerky playback)
			uint64_t delta_ns = now - ctx->last_render_time;
			if (ctx->last_render_time == 0) {
				delta_ns = 16666667; // Assume 60fps initially
			}
			double delta_s = (double)delta_ns / 1e9;

			// Max speed change: 0.5 per second (smoother than instant)
			float max_speed_change = 0.5f * (float)delta_s;
			float speed_diff = target_speed - ctx->current_speed;
			if (speed_diff > max_speed_change)
				speed_diff = max_speed_change;
			if (speed_diff < -max_speed_change)
				speed_diff = -max_speed_change;
			ctx->current_speed += speed_diff;

			ctx->last_render_time = now;

			// Accumulate RTP ticks based on current speed
			// Problem: Server RTP timestamps assume 30fps (3000 ticks/frame)
			//          But actual frame delivery is ~20fps
			// Solution: Scale playback rate to match actual arrival fps
			double actual_fps = jitter_estimator_get_fps(ctx->jitter_est);
			if (actual_fps < 10.0)
				actual_fps = 20.0; // Default to 20fps if not yet estimated

			// At 30fps: 90 ticks/ms. At 20fps: 60 ticks/ms
			double ticks_per_ms = 90.0 * (actual_fps / 30.0);
			ctx->accumulated_rtp += ((double)delta_ns / 1000000.0) * ticks_per_ms * ctx->current_speed;
			uint32_t current_rtp = ctx->jitter_playback_start_rtp + (uint32_t)ctx->accumulated_rtp;

// Frame management with interpolation
// Helper: pop oldest frame from sorted buffer (index 0)
#define POP_OLDEST_FRAME() do { \
				bfree(ctx->jitter_buffer[0].data); \
				ctx->jitter_buffer[0].data = NULL; \
				for (int _i = 0; _i < ctx->jitter_count - 1; _i++) { \
					ctx->jitter_buffer[_i] = ctx->jitter_buffer[_i + 1]; \
				} \
				ctx->jitter_buffer[ctx->jitter_count - 1].data = NULL; \
				ctx->jitter_count--; \
			} while(0)

			if (!ctx->interp_has_a && ctx->jitter_count > 0) {
				struct jitter_frame *jf = &ctx->jitter_buffer[0]; // Oldest is at index 0
				size_t frame_size = jf->size;

				if (!ctx->interp_frame_a || ctx->interp_width != jf->width ||
				    ctx->interp_height != jf->height) {
					bfree(ctx->interp_frame_a);
					bfree(ctx->interp_frame_b);
					bfree(ctx->interp_frame_out);
					ctx->interp_frame_a = bmalloc(frame_size);
					ctx->interp_frame_b = bmalloc(frame_size);
					ctx->interp_frame_out = bmalloc(frame_size);
					ctx->interp_width = jf->width;
					ctx->interp_height = jf->height;
				}

				memcpy(ctx->interp_frame_a, jf->data, frame_size);
				ctx->interp_rtp_a = jf->rtp_timestamp;
				ctx->interp_receive_a = jf->receive_time_ns;
				ctx->interp_has_a = true;

				POP_OLDEST_FRAME();
			}

			if (ctx->interp_has_a && !ctx->interp_has_b && ctx->jitter_count > 0) {
				struct jitter_frame *jf = &ctx->jitter_buffer[0]; // Oldest is at index 0
				memcpy(ctx->interp_frame_b, jf->data, jf->size);
				ctx->interp_rtp_b = jf->rtp_timestamp;
				ctx->interp_receive_b = jf->receive_time_ns;
				ctx->interp_has_b = true;

				// Log RTP timestamp gap to understand server's timing
				int32_t rtp_gap = (int32_t)(ctx->interp_rtp_b - ctx->interp_rtp_a);
				double rtp_gap_ms = (double)rtp_gap / 90.0; // 90kHz clock
				static int rtp_log_counter = 0;
				if (rtp_log_counter++ % 30 == 0) {
					blog(LOG_INFO, "[RTP] Frame gap: %.1fms (rtp_ticks=%d)", rtp_gap_ms, rtp_gap);
				}

				POP_OLDEST_FRAME();
			}

			// Advance frames if current time passed frame B
			if (ctx->interp_has_a && ctx->interp_has_b) {
				int32_t diff_to_b = (int32_t)(ctx->interp_rtp_b - current_rtp);

				if (diff_to_b <= 0) {
					memcpy(ctx->interp_frame_a, ctx->interp_frame_b,
					       ctx->interp_width * ctx->interp_height * 4);
					ctx->interp_rtp_a = ctx->interp_rtp_b;
					ctx->interp_receive_a = ctx->interp_receive_b;
					ctx->interp_has_b = false;

					if (ctx->jitter_count > 0) {
						struct jitter_frame *jf = &ctx->jitter_buffer[0]; // Oldest at index 0
						memcpy(ctx->interp_frame_b, jf->data, jf->size);
						ctx->interp_rtp_b = jf->rtp_timestamp;
						ctx->interp_receive_b = jf->receive_time_ns;
						ctx->interp_has_b = true;
						POP_OLDEST_FRAME();
					}
				}
			}

#undef POP_OLDEST_FRAME

			// Render interpolated frame
			pthread_mutex_lock(&ctx->mutex);

			if (ctx->interp_has_a) {
				size_t frame_size = ctx->interp_width * ctx->interp_height * 4;

				if (!ctx->decoded_frame || ctx->decoded_frame_width != ctx->interp_width ||
				    ctx->decoded_frame_height != ctx->interp_height) {
					bfree(ctx->decoded_frame);
					ctx->decoded_frame = bmalloc(frame_size);
					ctx->decoded_frame_width = ctx->interp_width;
					ctx->decoded_frame_height = ctx->interp_height;
				}

				if (ctx->interp_has_b) {
					int32_t rtp_range = (int32_t)(ctx->interp_rtp_b - ctx->interp_rtp_a);
					int32_t rtp_pos = (int32_t)(current_rtp - ctx->interp_rtp_a);

					float alpha = 0.0f;
					if (rtp_range > 0) {
						alpha = (float)rtp_pos / (float)rtp_range;
						if (alpha < 0.0f)
							alpha = 0.0f;
						if (alpha > 1.0f)
							alpha = 1.0f;
					}

					uint8_t *a = ctx->interp_frame_a;
					uint8_t *b = ctx->interp_frame_b;
					uint8_t *out = ctx->decoded_frame;
					size_t pixel_count = ctx->interp_width * ctx->interp_height * 4;

					for (size_t i = 0; i < pixel_count; i++) {
						out[i] = (uint8_t)((float)a[i] * (1.0f - alpha) + (float)b[i] * alpha);
					}

					static int log_counter = 0;
					if (log_counter++ % 30 == 0) {
						// Calculate actual frame latency: time since frame A was received
						uint64_t frame_age_ms = (now - ctx->interp_receive_a) / 1000000;
						double est_fps = jitter_estimator_get_fps(ctx->jitter_est);
						blog(LOG_INFO,
						     "[Smooth] buf=%d/%d, speed=%.2f, latency=%llums, est_fps=%.1f",
						     ctx->jitter_count, ctx->buffer_target, ctx->current_speed,
						     (unsigned long long)frame_age_ms, est_fps);
					}
				} else {
					memcpy(ctx->decoded_frame, ctx->interp_frame_a, frame_size);
				}

				ctx->decoded_frame_ready = true;
			}

			pthread_mutex_unlock(&ctx->mutex);
		}

		pthread_mutex_unlock(&ctx->jitter_mutex);

		// Now render the frame
		pthread_mutex_lock(&ctx->mutex);
		if (ctx->decoded_frame_ready && ctx->decoded_frame) {
			render_update_count++;

			if (!ctx->output_texture ||
			    gs_texture_get_width(ctx->output_texture) != ctx->decoded_frame_width ||
			    gs_texture_get_height(ctx->output_texture) != ctx->decoded_frame_height) {
				if (ctx->output_texture)
					gs_texture_destroy(ctx->output_texture);
				ctx->output_texture = gs_texture_create(ctx->decoded_frame_width,
									ctx->decoded_frame_height, GS_BGRA, 1, NULL,
									GS_DYNAMIC);
			}

			if (ctx->output_texture) {
				gs_texture_set_image(ctx->output_texture, ctx->decoded_frame,
						     ctx->decoded_frame_width * 4, false);
				output = ctx->output_texture;
			}
		}
		pthread_mutex_unlock(&ctx->mutex);
	} else {
		// Normal mode: use decoded_frame directly
		pthread_mutex_lock(&ctx->mutex);

		if (ctx->decoded_frame_ready && ctx->decoded_frame) {
			render_update_count++;
			uint64_t now = os_gettime_ns();
			uint64_t render_gap_ms =
				(last_render_update_time > 0) ? (now - last_render_update_time) / 1000000 : 0;

			if (render_gap_ms > 100 || render_update_count % 30 == 0) {
				blog(LOG_INFO, "[DEBUG Render] Frame update #%llu, gap=%llums",
				     (unsigned long long)render_update_count, (unsigned long long)render_gap_ms);
			}
			last_render_update_time = now;

			if (!ctx->output_texture ||
			    gs_texture_get_width(ctx->output_texture) != ctx->decoded_frame_width ||
			    gs_texture_get_height(ctx->output_texture) != ctx->decoded_frame_height) {
				if (ctx->output_texture)
					gs_texture_destroy(ctx->output_texture);
				ctx->output_texture = gs_texture_create(ctx->decoded_frame_width,
									ctx->decoded_frame_height, GS_BGRA, 1, NULL,
									GS_DYNAMIC);
			}

			if (ctx->output_texture) {
				gs_texture_set_image(ctx->output_texture, ctx->decoded_frame,
						     ctx->decoded_frame_width * 4, false);
				output = ctx->output_texture;
			}
		} else if (ctx->streaming) {
			render_no_frame_count++;
			if (render_no_frame_count % 100 == 0) {
				blog(LOG_INFO, "[DEBUG Render] No frame available (count=%llu)",
				     (unsigned long long)render_no_frame_count);
			}
		}
		pthread_mutex_unlock(&ctx->mutex);
	}
	// === END DEBUG ===

	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(default_effect, "Draw");

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	if (ctx->streaming && output == ctx->output_texture && ctx->decoded_frame_ready) {
		float scale;
		if (parent_width < parent_height) {
			scale = (float)STREAM_SIZE / (float)parent_width;
		} else {
			scale = (float)STREAM_SIZE / (float)parent_height;
		}

		float render_size = STREAM_SIZE / scale;
		float render_x = (ctx->width - render_size) / 2.0f;
		float render_y = (ctx->height - render_size) / 2.0f;

		gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), output);
		gs_matrix_push();
		gs_matrix_translate3f(render_x, render_y, 0.0f);
		gs_draw_sprite(output, 0, (uint32_t)render_size, (uint32_t)render_size);
		gs_matrix_pop();
	} else {
		gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), output);
		gs_draw_sprite(output, 0, ctx->width, ctx->height);
	}

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static uint32_t daydream_filter_get_width(void *data)
{
	struct daydream_filter *ctx = data;
	return ctx->width;
}

static uint32_t daydream_filter_get_height(void *data)
{
	struct daydream_filter *ctx = data;
	return ctx->height;
}

static void on_login_callback(bool success, const char *api_key, const char *error, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(api_key);

	if (success) {
		blog(LOG_INFO, "[Daydream] Login successful!");
	} else {
		blog(LOG_ERROR, "[Daydream] Login failed: %s", error ? error : "Unknown error");
	}

	obs_source_update_properties(ctx->source);
}

static bool on_login_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	if (daydream_auth_is_logged_in(ctx->auth)) {
		blog(LOG_INFO, "[Daydream] Already logged in");
		return false;
	}

	daydream_auth_login(ctx->auth, on_login_callback, ctx);

	return false;
}

static bool on_logout_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	daydream_auth_logout(ctx->auth);
	obs_source_update_properties(ctx->source);

	return true;
}

static void *start_streaming_thread_func(void *data)
{
	struct daydream_filter *ctx = data;

	blog(LOG_INFO, "[Daydream] Start streaming thread started");

	const uint32_t STREAM_SIZE = 512;

	pthread_mutex_lock(&ctx->mutex);
	const char *api_key = daydream_auth_get_api_key(ctx->auth);
	char *api_key_copy = api_key ? bstrdup(api_key) : NULL;

	struct daydream_stream_params params = {
		.model_id = ctx->model ? bstrdup(ctx->model) : NULL,
		.prompt = ctx->prompt ? bstrdup(ctx->prompt) : NULL,
		.negative_prompt = ctx->negative_prompt ? bstrdup(ctx->negative_prompt) : NULL,
		.guidance = ctx->guidance,
		.delta = ctx->delta,
		.steps = ctx->steps,
		.width = (int)STREAM_SIZE,
		.height = (int)STREAM_SIZE,
	};
	uint32_t target_fps = ctx->target_fps;
	pthread_mutex_unlock(&ctx->mutex);

	struct daydream_stream_result result = daydream_api_create_stream(api_key_copy, &params);

	bfree((char *)params.model_id);
	bfree((char *)params.prompt);
	bfree((char *)params.negative_prompt);

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->stopping) {
		blog(LOG_INFO, "[Daydream] Streaming cancelled");
		bfree(api_key_copy);
		daydream_api_free_result(&result);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	if (!result.success) {
		blog(LOG_ERROR, "[Daydream] Failed to create stream: %s",
		     result.error ? result.error : "Unknown error");
		bfree(api_key_copy);
		daydream_api_free_result(&result);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);

	ctx->stream_id = bstrdup(result.stream_id);
	ctx->whip_url = bstrdup(result.whip_url);
	ctx->whep_url = NULL;

	blog(LOG_INFO, "[Daydream] Stream created: %s", ctx->stream_id);
	blog(LOG_INFO, "[Daydream] WHIP URL: %s", ctx->whip_url);

	struct daydream_encoder_config enc_config = {
		.width = STREAM_SIZE,
		.height = STREAM_SIZE,
		.fps = target_fps,
		.bitrate = 500000,
	};
	ctx->encoder = daydream_encoder_create(&enc_config);
	if (!ctx->encoder) {
		blog(LOG_ERROR, "[Daydream] Failed to create encoder");
		bfree(api_key_copy);
		daydream_api_free_result(&result);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	struct daydream_decoder_config dec_config = {
		.width = STREAM_SIZE,
		.height = STREAM_SIZE,
	};
	ctx->decoder = daydream_decoder_create(&dec_config);
	if (!ctx->decoder) {
		blog(LOG_ERROR, "[Daydream] Failed to create decoder");
		daydream_encoder_destroy(ctx->encoder);
		ctx->encoder = NULL;
		bfree(api_key_copy);
		daydream_api_free_result(&result);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	struct daydream_whip_config whip_config = {
		.whip_url = ctx->whip_url,
		.api_key = api_key_copy,
		.width = STREAM_SIZE,
		.height = STREAM_SIZE,
		.fps = target_fps,
		.on_state = on_whip_state,
		.userdata = ctx,
	};
	ctx->whip = daydream_whip_create(&whip_config);
	pthread_mutex_unlock(&ctx->mutex);

	if (!daydream_whip_connect(ctx->whip)) {
		blog(LOG_ERROR, "[Daydream] Failed to connect WHIP");
		pthread_mutex_lock(&ctx->mutex);
		daydream_whip_destroy(ctx->whip);
		ctx->whip = NULL;
		daydream_encoder_destroy(ctx->encoder);
		ctx->encoder = NULL;
		daydream_decoder_destroy(ctx->decoder);
		ctx->decoder = NULL;
		bfree(api_key_copy);
		daydream_api_free_result(&result);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	pthread_mutex_lock(&ctx->mutex);
	ctx->streaming = true;
	ctx->stopping = false;
	ctx->frame_count = 0;
	ctx->last_encode_time = os_gettime_ns();

	ctx->encode_thread_running = true;
	pthread_create(&ctx->encode_thread, NULL, encode_thread_func, ctx);

	ctx->decode_thread_running = true;
	pthread_create(&ctx->decode_thread, NULL, decode_thread_func, ctx);

	blog(LOG_INFO, "[Daydream] WHIP streaming started, connecting WHEP in background...");

	const char *whep_url = daydream_whip_get_whep_url(ctx->whip);
	if (whep_url) {
		ctx->whep_url = bstrdup(whep_url);
		blog(LOG_INFO, "[Daydream] WHEP URL: %s", ctx->whep_url);

		struct daydream_whep_config whep_config = {
			.whep_url = ctx->whep_url,
			.api_key = NULL,
			.on_frame = on_whep_frame,
			.on_state = on_whep_state,
			.userdata = ctx,
		};
		ctx->whep = daydream_whep_create(&whep_config);

		ctx->whep_thread_running = true;
		pthread_create(&ctx->whep_thread, NULL, whep_connect_thread_func, ctx);
	} else {
		blog(LOG_WARNING, "[Daydream] No WHEP URL available, output will not be received");
	}

	bfree(api_key_copy);
	daydream_api_free_result(&result);
	ctx->start_thread_running = false;
	pthread_mutex_unlock(&ctx->mutex);

	blog(LOG_INFO, "[Daydream] Streaming started!");

	return NULL;
}

static bool on_start_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->streaming || ctx->start_thread_running) {
		blog(LOG_WARNING, "[Daydream] Already streaming or starting");
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	const char *api_key = daydream_auth_get_api_key(ctx->auth);
	if (!api_key || strlen(api_key) == 0) {
		blog(LOG_ERROR, "[Daydream] Not logged in. Please login first.");
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	ctx->start_thread_running = true;
	ctx->stopping = false;
	pthread_create(&ctx->start_thread, NULL, start_streaming_thread_func, ctx);

	pthread_mutex_unlock(&ctx->mutex);

	blog(LOG_INFO, "[Daydream] Starting stream in background...");

	return false;
}

static bool on_stop_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (!ctx->streaming) {
		blog(LOG_WARNING, "[Daydream] Not streaming");
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	pthread_mutex_unlock(&ctx->mutex);

	stop_streaming(ctx);

	pthread_mutex_lock(&ctx->mutex);
	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);
	ctx->stream_id = NULL;
	ctx->whip_url = NULL;
	ctx->whep_url = NULL;
	pthread_mutex_unlock(&ctx->mutex);

	blog(LOG_INFO, "[Daydream] Streaming stopped");

	return false;
}

static obs_properties_t *daydream_filter_get_properties(void *data)
{
	struct daydream_filter *ctx = data;

	obs_properties_t *props = obs_properties_create();

	bool logged_in = daydream_auth_is_logged_in(ctx->auth);

	if (logged_in) {
		obs_properties_add_text(props, PROP_LOGIN_STATUS, "Status: Logged In", OBS_TEXT_INFO);
		obs_properties_add_button(props, PROP_LOGOUT, "Logout", on_logout_clicked);
	} else {
		obs_properties_add_text(props, PROP_LOGIN_STATUS, "Status: Not Logged In", OBS_TEXT_INFO);
		obs_properties_add_button(props, PROP_LOGIN, "Login with Daydream", on_login_clicked);
	}

	obs_property_t *model =
		obs_properties_add_list(props, PROP_MODEL, "Model", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(model, "SDXL Turbo", "stabilityai/sdxl-turbo");
	obs_property_list_add_string(model, "SD Turbo", "stabilityai/sd-turbo");
	obs_property_list_add_string(model, "Dreamshaper 8", "Lykon/dreamshaper-8");
	obs_property_list_add_string(model, "Openjourney v4", "prompthero/openjourney-v4");
	obs_property_set_enabled(model, logged_in);

	obs_property_t *prompt = obs_properties_add_text(props, PROP_PROMPT, "Prompt", OBS_TEXT_MULTILINE);
	obs_property_set_enabled(prompt, logged_in);

	obs_property_t *neg_prompt =
		obs_properties_add_text(props, PROP_NEGATIVE_PROMPT, "Negative Prompt", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(neg_prompt, logged_in);

	obs_property_t *guidance = obs_properties_add_float_slider(props, PROP_GUIDANCE, "Guidance", 0.1, 20.0, 0.1);
	obs_property_set_enabled(guidance, logged_in);

	obs_property_t *delta = obs_properties_add_float_slider(props, PROP_DELTA, "Delta", 0.0, 1.0, 0.01);
	obs_property_set_enabled(delta, logged_in);

	obs_property_t *steps = obs_properties_add_int_slider(props, PROP_STEPS, "Steps", 1, 100, 1);
	obs_property_set_enabled(steps, logged_in);

	obs_property_t *smooth = obs_properties_add_bool(props, PROP_SMOOTH_MODE, "Smooth Mode");
	obs_property_set_enabled(smooth, logged_in);

	// Adaptive rate control parameters
	obs_property_t *buffer_target =
		obs_properties_add_int_slider(props, PROP_BUFFER_TARGET, "Buffer Target (frames)", 5, 30, 1);
	obs_property_set_enabled(buffer_target, logged_in);

	obs_property_t *adapt_speed =
		obs_properties_add_float_slider(props, PROP_ADAPT_SPEED, "Adapt Speed", 0.01, 0.5, 0.01);
	obs_property_set_enabled(adapt_speed, logged_in);

	obs_property_t *speed_min = obs_properties_add_float_slider(props, PROP_SPEED_MIN, "Min Speed", 0.1, 1.0, 0.05);
	obs_property_set_enabled(speed_min, logged_in);

	obs_property_t *speed_max = obs_properties_add_float_slider(props, PROP_SPEED_MAX, "Max Speed", 1.0, 3.0, 0.1);
	obs_property_set_enabled(speed_max, logged_in);

	obs_property_t *start = obs_properties_add_button(props, PROP_START, "Start Streaming", on_start_clicked);
	obs_property_set_enabled(start, logged_in && !ctx->streaming);

	obs_property_t *stop = obs_properties_add_button(props, PROP_STOP, "Stop Streaming", on_stop_clicked);
	obs_property_set_enabled(stop, logged_in && ctx->streaming);

	return props;
}

static void daydream_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, PROP_MODEL, "stabilityai/sdxl-turbo");
	obs_data_set_default_string(settings, PROP_PROMPT, "a beautiful landscape");
	obs_data_set_default_string(settings, PROP_NEGATIVE_PROMPT, "blurry, low quality");
	obs_data_set_default_double(settings, PROP_GUIDANCE, 1.0);
	obs_data_set_default_double(settings, PROP_DELTA, 0.7);
	obs_data_set_default_int(settings, PROP_STEPS, 50);
	obs_data_set_default_bool(settings, PROP_SMOOTH_MODE, false);

	// Adaptive rate control defaults - tuned for ultra low latency
	obs_data_set_default_int(settings, PROP_BUFFER_TARGET, 4);    // Ultra low latency (~200ms)
	obs_data_set_default_double(settings, PROP_ADAPT_SPEED, 0.3); // Very fast adaptation
	obs_data_set_default_double(settings, PROP_SPEED_MIN, 0.1);   // Can almost pause
	obs_data_set_default_double(settings, PROP_SPEED_MAX, 3.0);   // Very fast catchup
}

static struct obs_source_info daydream_filter_info = {
	.id = "daydream_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = daydream_filter_get_name,
	.create = daydream_filter_create,
	.destroy = daydream_filter_destroy,
	.update = daydream_filter_update,
	.get_width = daydream_filter_get_width,
	.get_height = daydream_filter_get_height,
	.video_render = daydream_filter_video_render,
	.get_properties = daydream_filter_get_properties,
	.get_defaults = daydream_filter_get_defaults,
};

void daydream_filter_register(void)
{
	obs_register_source(&daydream_filter_info);
}
