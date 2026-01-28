#include "daydream-filter.h"
#include "daydream-api.h"
#include "daydream-auth.h"
#include "daydream-encoder.h"
#include "daydream-decoder.h"
#include "daydream-whip.h"
#include "daydream-whep.h"
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

#define FRAME_QUEUE_SIZE 16
#define RAW_QUEUE_SIZE 32
#define FRAME_INTERVAL_NS (1000000000ULL / 30)
#define JITTER_HISTORY_SIZE 16
#define MIN_BUFFER_FRAMES 3
#define MAX_BUFFER_FRAMES 12

struct frame_entry {
	uint8_t *data;
	uint32_t width;
	uint32_t height;
	uint32_t linesize;
};

struct raw_packet {
	uint8_t *data;
	size_t size;
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

	gs_texrender_t *blur_texrender;

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

	struct frame_entry frame_queue[FRAME_QUEUE_SIZE];
	int queue_head;
	int queue_tail;
	int queue_count;
	bool queue_started;
	uint64_t last_output_time;

	uint64_t jitter_history[JITTER_HISTORY_SIZE];
	int jitter_idx;
	uint64_t last_receive_time;
	int target_buffer_frames;
	int underrun_count;

	pthread_mutex_t mutex;
	pthread_cond_t frame_cond;

	uint64_t frame_count;
	uint64_t last_encode_time;
	uint32_t target_fps;
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

	pthread_mutex_unlock(&ctx->mutex);
}

static void on_whep_frame(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(timestamp);
	UNUSED_PARAMETER(is_keyframe);

	if (!ctx)
		return;

	pthread_mutex_lock(&ctx->raw_mutex);

	if (ctx->raw_queue_count >= RAW_QUEUE_SIZE) {
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
		memcpy(raw_data, pkt->data, raw_size);

		ctx->raw_queue_tail = (ctx->raw_queue_tail + 1) % RAW_QUEUE_SIZE;
		ctx->raw_queue_count--;

		pthread_mutex_unlock(&ctx->raw_mutex);

		uint64_t decode_start = os_gettime_ns();
		struct daydream_decoded_frame decoded;
		if (ctx->decoder && daydream_decoder_decode(ctx->decoder, raw_data, raw_size, &decoded)) {
			uint64_t decode_time = (os_gettime_ns() - decode_start) / 1000000;
			if (decode_time > 15) {
				blog(LOG_WARNING, "[Daydream Decode] Slow decode: %llums, size=%zu",
				     (unsigned long long)decode_time, raw_size);
			}
			pthread_mutex_lock(&ctx->mutex);

			uint64_t now = os_gettime_ns();
			if (ctx->last_receive_time > 0) {
				uint64_t delta = now - ctx->last_receive_time;
				ctx->jitter_history[ctx->jitter_idx] = delta;
				ctx->jitter_idx = (ctx->jitter_idx + 1) % JITTER_HISTORY_SIZE;

				uint64_t max_delta = 0;
				for (int i = 0; i < JITTER_HISTORY_SIZE; i++) {
					if (ctx->jitter_history[i] > max_delta)
						max_delta = ctx->jitter_history[i];
				}

				int needed_frames = (int)((max_delta + FRAME_INTERVAL_NS - 1) / FRAME_INTERVAL_NS) + 1;
				if (needed_frames < MIN_BUFFER_FRAMES)
					needed_frames = MIN_BUFFER_FRAMES;
				if (needed_frames > MAX_BUFFER_FRAMES)
					needed_frames = MAX_BUFFER_FRAMES;

				if (ctx->underrun_count > 0)
					needed_frames += ctx->underrun_count;
				if (needed_frames > MAX_BUFFER_FRAMES)
					needed_frames = MAX_BUFFER_FRAMES;

				ctx->target_buffer_frames = needed_frames;
			}
			ctx->last_receive_time = now;

			if (ctx->queue_count >= FRAME_QUEUE_SIZE) {
				ctx->queue_tail = (ctx->queue_tail + 1) % FRAME_QUEUE_SIZE;
				ctx->queue_count--;
			}

			struct frame_entry *entry = &ctx->frame_queue[ctx->queue_head];
			size_t frame_size = decoded.linesize * decoded.height;

			if (!entry->data || entry->width != decoded.width || entry->height != decoded.height) {
				bfree(entry->data);
				entry->data = bmalloc(frame_size);
			}

			memcpy(entry->data, decoded.data, frame_size);
			entry->width = decoded.width;
			entry->height = decoded.height;
			entry->linesize = decoded.linesize;

			ctx->queue_head = (ctx->queue_head + 1) % FRAME_QUEUE_SIZE;
			ctx->queue_count++;

			pthread_mutex_unlock(&ctx->mutex);
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
	uint64_t last_debug_time = 0;
	uint64_t frames_sent = 0;

	blog(LOG_INFO, "[Daydream Encode] Thread started, target_fps=%u", ctx->target_fps);

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

		uint32_t frame_width = ctx->pending_frame_width;
		uint32_t frame_height = ctx->pending_frame_height;
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
				frames_sent++;
			}
		}

		uint64_t now = os_gettime_ns();
		if (now - last_debug_time > 1000000000ULL) {
			blog(LOG_INFO, "[Daydream Encode] frame=%dx%d encoder=%d whip=%d connected=%d sent=%llu",
			     frame_width, frame_height, has_encoder, has_whip, whip_connected,
			     (unsigned long long)frames_sent);
			last_debug_time = now;
		}

		bfree(frame_copy);

		uint64_t elapsed = now - ctx->last_encode_time;
		if (elapsed < frame_interval_ns) {
			os_sleepto_ns(ctx->last_encode_time + frame_interval_ns);
		}
		ctx->last_encode_time = os_gettime_ns();
	}

	blog(LOG_INFO, "[Daydream Encode] Thread stopped, total frames sent: %llu", (unsigned long long)frames_sent);

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

	ctx->queue_head = 0;
	ctx->queue_tail = 0;
	ctx->queue_count = 0;
	ctx->queue_started = false;
	ctx->decoded_frame_ready = false;

	ctx->jitter_idx = 0;
	ctx->last_receive_time = 0;
	ctx->target_buffer_frames = MIN_BUFFER_FRAMES;
	ctx->underrun_count = 0;
	for (int i = 0; i < JITTER_HISTORY_SIZE; i++)
		ctx->jitter_history[i] = 0;

	ctx->raw_queue_head = 0;
	ctx->raw_queue_tail = 0;
	ctx->raw_queue_count = 0;
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
	if (ctx->blur_texrender)
		gs_texrender_destroy(ctx->blur_texrender);
	obs_leave_graphics();

	daydream_auth_destroy(ctx->auth);

	bfree(ctx->prompt);
	bfree(ctx->negative_prompt);
	bfree(ctx->model);
	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);
	bfree(ctx->pending_frame);
	bfree(ctx->decoded_frame);

	for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
		bfree(ctx->frame_queue[i].data);
	}

	for (int i = 0; i < RAW_QUEUE_SIZE; i++) {
		bfree(ctx->raw_queue[i].data);
	}

	pthread_cond_destroy(&ctx->frame_cond);
	pthread_mutex_destroy(&ctx->mutex);
	pthread_cond_destroy(&ctx->raw_cond);
	pthread_mutex_destroy(&ctx->raw_mutex);

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

	pthread_mutex_lock(&ctx->mutex);

	static uint64_t last_queue_log = 0;
	uint64_t now_ns = os_gettime_ns();
	if (now_ns - last_queue_log > 1000000000ULL) {
		blog(LOG_INFO, "[Daydream] Queue: count=%d repeats=%d", ctx->queue_count, ctx->underrun_count);
		last_queue_log = now_ns;
	}

	if (!ctx->queue_started && ctx->queue_count >= MIN_BUFFER_FRAMES) {
		ctx->queue_started = true;
		ctx->last_output_time = os_gettime_ns();
		blog(LOG_INFO, "[Daydream] Frame queue started with %d frames buffered", ctx->queue_count);
	}

	if (ctx->queue_started) {
		uint64_t now = os_gettime_ns();
		bool should_pop = false;

		if (ctx->queue_count >= 6) {
			should_pop = true;
		} else if (ctx->queue_count > 0 && now - ctx->last_output_time >= FRAME_INTERVAL_NS) {
			should_pop = true;
		}

		if (should_pop && ctx->queue_count > 0) {
			struct frame_entry *entry = &ctx->frame_queue[ctx->queue_tail];

			size_t frame_size = entry->linesize * entry->height;
			if (!ctx->decoded_frame || ctx->decoded_frame_width != entry->width ||
			    ctx->decoded_frame_height != entry->height) {
				bfree(ctx->decoded_frame);
				ctx->decoded_frame = bmalloc(frame_size);
			}

			memcpy(ctx->decoded_frame, entry->data, frame_size);
			ctx->decoded_frame_width = entry->width;
			ctx->decoded_frame_height = entry->height;
			ctx->decoded_frame_ready = true;

			ctx->queue_tail = (ctx->queue_tail + 1) % FRAME_QUEUE_SIZE;
			ctx->queue_count--;
			ctx->last_output_time = now;
		}
	}

	uint64_t tex_start = os_gettime_ns();
	if (ctx->decoded_frame_ready && ctx->decoded_frame) {
		if (!ctx->output_texture || gs_texture_get_width(ctx->output_texture) != ctx->decoded_frame_width ||
		    gs_texture_get_height(ctx->output_texture) != ctx->decoded_frame_height) {
			if (ctx->output_texture)
				gs_texture_destroy(ctx->output_texture);
			ctx->output_texture = gs_texture_create(ctx->decoded_frame_width, ctx->decoded_frame_height,
								GS_BGRA, 1, NULL, GS_DYNAMIC);
		}

		if (ctx->output_texture) {
			gs_texture_set_image(ctx->output_texture, ctx->decoded_frame, ctx->decoded_frame_width * 4,
					     false);
			output = ctx->output_texture;
		}
	}
	pthread_mutex_unlock(&ctx->mutex);

	uint64_t tex_time = (os_gettime_ns() - tex_start) / 1000000;
	static uint64_t last_tex_warn = 0;
	if (tex_time > 10 && os_gettime_ns() - last_tex_warn > 500000000ULL) {
		blog(LOG_WARNING, "[Daydream Render] Slow texture upload: %llums", (unsigned long long)tex_time);
		last_tex_warn = os_gettime_ns();
	}

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
