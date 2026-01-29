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

struct daydream_filter {
	obs_source_t *source;

	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	gs_texture_t *output_texture;
	uint32_t width;
	uint32_t height;

	gs_texrender_t *crop_texrender;
	gs_stagesurf_t *crop_stagesurface;

#if defined(__APPLE__)
	// Zero-copy encoding (IOSurface-backed texture)
	gs_texture_t *iosurface_texture;
	bool use_zerocopy;
#endif

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

	pthread_t whep_thread;
	bool whep_thread_running;

	pthread_t start_thread;
	bool start_thread_running;

	// Double buffer for zero-copy encode
	uint8_t *pending_frame[2];
	uint32_t pending_frame_width;
	uint32_t pending_frame_height;
	uint32_t pending_frame_linesize;
	int pending_produce_idx; // Buffer index with ready data
	int pending_consume_idx; // Buffer index encode is reading (-1 if idle)
	bool pending_frame_ready;

	// Double buffer for decode output
	uint8_t *decoded_frame[2]; // BGRA fallback
	uint8_t *nv12_y_data[2];   // NV12 Y plane
	uint8_t *nv12_uv_data[2];  // NV12 UV plane
	uint32_t decoded_frame_width;
	uint32_t decoded_frame_height;
	uint32_t nv12_y_linesize;
	uint32_t nv12_uv_linesize;
	int decode_produce_idx; // Buffer index with ready data
	int decode_consume_idx; // Buffer index render is reading (-1 if idle)
	bool decoded_frame_ready;
	bool decoded_frame_is_nv12;

	// NV12 GPU conversion
	gs_texture_t *nv12_tex_y;
	gs_texture_t *nv12_tex_uv;
	gs_effect_t *nv12_effect;
	gs_texrender_t *nv12_texrender;

	pthread_mutex_t mutex;
	pthread_cond_t frame_cond;

	uint64_t frame_count;
	uint64_t last_encode_time;
	uint32_t target_fps;

	// Adaptive bitrate control
	uint32_t current_bitrate;
	uint32_t min_bitrate;
	uint32_t max_bitrate;
	int32_t last_rtt_ms;
	int32_t avg_rtt_ms;
	uint64_t last_bitrate_check;
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

static void on_whep_frame(const uint8_t *data, size_t size, uint32_t rtp_timestamp, bool is_keyframe, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(is_keyframe);
	UNUSED_PARAMETER(rtp_timestamp);

	if (!ctx || !ctx->decoder || ctx->stopping)
		return;

	struct daydream_decoded_frame decoded;
	if (!daydream_decoder_decode(ctx->decoder, data, size, &decoded))
		return;

	pthread_mutex_lock(&ctx->mutex);

	// Write to buffer that render isn't reading
	int write_idx = (ctx->decode_consume_idx == 0) ? 1 : 0;

	if (decoded.is_nv12) {
		size_t y_size = decoded.y_linesize * decoded.height;
		size_t uv_size = decoded.uv_linesize * (decoded.height / 2);

		// Allocate double buffers if needed
		if (!ctx->nv12_y_data[0] || ctx->decoded_frame_width != decoded.width ||
		    ctx->decoded_frame_height != decoded.height) {
			bfree(ctx->nv12_y_data[0]);
			bfree(ctx->nv12_y_data[1]);
			bfree(ctx->nv12_uv_data[0]);
			bfree(ctx->nv12_uv_data[1]);
			ctx->nv12_y_data[0] = bmalloc(y_size);
			ctx->nv12_y_data[1] = bmalloc(y_size);
			ctx->nv12_uv_data[0] = bmalloc(uv_size);
			ctx->nv12_uv_data[1] = bmalloc(uv_size);
		}

		memcpy(ctx->nv12_y_data[write_idx], decoded.y_data, y_size);
		memcpy(ctx->nv12_uv_data[write_idx], decoded.uv_data, uv_size);
		ctx->nv12_y_linesize = decoded.y_linesize;
		ctx->nv12_uv_linesize = decoded.uv_linesize;
		ctx->decoded_frame_is_nv12 = true;
	} else {
		size_t frame_size = decoded.bgra_linesize * decoded.height;

		// Allocate double buffers if needed
		if (!ctx->decoded_frame[0] || ctx->decoded_frame_width != decoded.width ||
		    ctx->decoded_frame_height != decoded.height) {
			bfree(ctx->decoded_frame[0]);
			bfree(ctx->decoded_frame[1]);
			ctx->decoded_frame[0] = bmalloc(frame_size);
			ctx->decoded_frame[1] = bmalloc(frame_size);
		}

		memcpy(ctx->decoded_frame[write_idx], decoded.bgra_data, frame_size);
		ctx->decoded_frame_is_nv12 = false;
	}

	ctx->decoded_frame_width = decoded.width;
	ctx->decoded_frame_height = decoded.height;
	ctx->decode_produce_idx = write_idx;
	ctx->decoded_frame_ready = true;

	pthread_mutex_unlock(&ctx->mutex);
}

static void on_whep_state(bool connected, const char *error, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(error);
	UNUSED_PARAMETER(connected);
}

static void on_whip_state(bool connected, const char *error, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(error);
	UNUSED_PARAMETER(connected);
}

static void *whep_connect_thread_func(void *data)
{
	struct daydream_filter *ctx = data;
	daydream_whep_connect(ctx->whep);
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

#if defined(__APPLE__)
		bool zerocopy = ctx->use_zerocopy;
#else
		bool zerocopy = false;
#endif
		uint8_t *frame_data = NULL;
		uint32_t frame_linesize = 0;

		if (!zerocopy) {
			// Take ownership of the buffer - no copy needed!
			ctx->pending_consume_idx = ctx->pending_produce_idx;
			frame_data = ctx->pending_frame[ctx->pending_consume_idx];
			frame_linesize = ctx->pending_frame_linesize;
		}
		ctx->pending_frame_ready = false;

		pthread_mutex_unlock(&ctx->mutex);

		if (ctx->encoder && ctx->whip && daydream_whip_is_connected(ctx->whip)) {
			struct daydream_encoded_frame encoded;
			bool success = false;

#if defined(__APPLE__)
			if (zerocopy) {
				success = daydream_encoder_encode_iosurface(ctx->encoder, &encoded);
			} else
#endif
			{
				success = daydream_encoder_encode(ctx->encoder, frame_data, frame_linesize, &encoded);
			}

			if (success) {
				uint32_t timestamp_ms = (uint32_t)(ctx->frame_count * 1000 / ctx->target_fps);
				daydream_whip_send_frame(ctx->whip, encoded.data, encoded.size, timestamp_ms,
							 encoded.is_keyframe);
				ctx->frame_count++;
			}

			// Adaptive bitrate: check RTT every second
			uint64_t now_ns = os_gettime_ns();
			if (now_ns - ctx->last_bitrate_check > 1000000000ULL) {
				ctx->last_bitrate_check = now_ns;

				int32_t rtt_ms = daydream_whip_get_rtt_ms(ctx->whip);
				if (rtt_ms > 0) {
					// Exponential moving average of RTT
					if (ctx->avg_rtt_ms <= 0) {
						ctx->avg_rtt_ms = rtt_ms;
					} else {
						ctx->avg_rtt_ms = (ctx->avg_rtt_ms * 7 + rtt_ms) / 8;
					}

					uint32_t new_bitrate = ctx->current_bitrate;

					// RTT thresholds for bitrate adjustment
					if (ctx->avg_rtt_ms > 300) {
						// High latency: reduce bitrate by 20%
						new_bitrate = ctx->current_bitrate * 80 / 100;
						blog(LOG_INFO, "[Daydream] High RTT (%d ms), reducing bitrate",
						     ctx->avg_rtt_ms);
					} else if (ctx->avg_rtt_ms > 150) {
						// Medium latency: reduce bitrate by 10%
						new_bitrate = ctx->current_bitrate * 90 / 100;
					} else if (ctx->avg_rtt_ms < 50 && ctx->last_rtt_ms < 50) {
						// Low latency for 2 checks: increase bitrate by 5%
						new_bitrate = ctx->current_bitrate * 105 / 100;
					}

					// Clamp to min/max
					if (new_bitrate < ctx->min_bitrate)
						new_bitrate = ctx->min_bitrate;
					if (new_bitrate > ctx->max_bitrate)
						new_bitrate = ctx->max_bitrate;

					// Apply new bitrate if changed significantly (>5%)
					if (new_bitrate != ctx->current_bitrate) {
						int diff = (int)new_bitrate - (int)ctx->current_bitrate;
						if (diff < 0)
							diff = -diff;
						if (diff > (int)(ctx->current_bitrate / 20)) {
							daydream_encoder_set_bitrate(ctx->encoder, new_bitrate);
							ctx->current_bitrate = new_bitrate;
						}
					}

					ctx->last_rtt_ms = rtt_ms;
				}
			}
		}

		// Release buffer ownership
		pthread_mutex_lock(&ctx->mutex);
		ctx->pending_consume_idx = -1;
		pthread_mutex_unlock(&ctx->mutex);

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
	ctx->pending_consume_idx = -1;
	ctx->decode_consume_idx = -1;

	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->frame_cond, NULL);

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

#if defined(__APPLE__)
	if (ctx->iosurface_texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->iosurface_texture);
		obs_leave_graphics();
		ctx->iosurface_texture = NULL;
	}
	ctx->use_zerocopy = false;
#endif

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
	if (ctx->nv12_tex_y)
		gs_texture_destroy(ctx->nv12_tex_y);
	if (ctx->nv12_tex_uv)
		gs_texture_destroy(ctx->nv12_tex_uv);
	if (ctx->nv12_effect)
		gs_effect_destroy(ctx->nv12_effect);
	if (ctx->nv12_texrender)
		gs_texrender_destroy(ctx->nv12_texrender);
	obs_leave_graphics();

	daydream_auth_destroy(ctx->auth);

	bfree(ctx->prompt);
	bfree(ctx->negative_prompt);
	bfree(ctx->model);
	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);
	bfree(ctx->pending_frame[0]);
	bfree(ctx->pending_frame[1]);
	bfree(ctx->decoded_frame[0]);
	bfree(ctx->decoded_frame[1]);
	bfree(ctx->nv12_y_data[0]);
	bfree(ctx->nv12_y_data[1]);
	bfree(ctx->nv12_uv_data[0]);
	bfree(ctx->nv12_uv_data[1]);

	pthread_cond_destroy(&ctx->frame_cond);
	pthread_mutex_destroy(&ctx->mutex);

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

	// Capture and send frames when streaming
	if (ctx->streaming && ctx->encode_thread_running) {
#if defined(__APPLE__)
		// Create IOSurface texture on first frame (must be done in render thread)
		if (ctx->use_zerocopy && !ctx->iosurface_texture && ctx->encoder) {
			IOSurfaceRef iosurface = daydream_encoder_get_iosurface(ctx->encoder);
			if (iosurface) {
				ctx->iosurface_texture = gs_texture_create_from_iosurface(iosurface);
				if (ctx->iosurface_texture) {
					blog(LOG_INFO, "[Daydream] IOSurface texture created in render thread");
				} else {
					blog(LOG_WARNING,
					     "[Daydream] Failed to create IOSurface texture, falling back");
					ctx->use_zerocopy = false;
				}
			} else {
				blog(LOG_WARNING, "[Daydream] No IOSurface available, falling back");
				ctx->use_zerocopy = false;
			}
		}
#endif

		float scale = (parent_width < parent_height) ? (float)STREAM_SIZE / (float)parent_width
							     : (float)STREAM_SIZE / (float)parent_height;

		float scaled_width = parent_width * scale;
		float scaled_height = parent_height * scale;
		float offset_x = (scaled_width - STREAM_SIZE) / 2.0f;
		float offset_y = (scaled_height - STREAM_SIZE) / 2.0f;

#if defined(__APPLE__)
		// Zero-copy path: render directly to IOSurface texture
		if (ctx->use_zerocopy && ctx->iosurface_texture) {
			gs_set_render_target(ctx->iosurface_texture, NULL);
			gs_set_viewport(0, 0, STREAM_SIZE, STREAM_SIZE);

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

			gs_set_render_target(NULL, NULL);

			// Signal encode thread - no CPU copy needed!
			pthread_mutex_lock(&ctx->mutex);
			ctx->pending_frame_ready = true;
			pthread_cond_signal(&ctx->frame_cond);
			pthread_mutex_unlock(&ctx->mutex);
		} else
#endif
		{
			// Standard path: copy to CPU buffer
			if (!ctx->crop_texrender)
				ctx->crop_texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
			if (!ctx->crop_stagesurface)
				ctx->crop_stagesurface = gs_stagesurface_create(STREAM_SIZE, STREAM_SIZE, GS_BGRA);

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

					// Allocate double buffers if needed
					if (!ctx->pending_frame[0] || ctx->pending_frame_width != STREAM_SIZE ||
					    ctx->pending_frame_height != STREAM_SIZE) {
						bfree(ctx->pending_frame[0]);
						bfree(ctx->pending_frame[1]);
						ctx->pending_frame[0] = bmalloc(data_size);
						ctx->pending_frame[1] = bmalloc(data_size);
						ctx->pending_frame_width = STREAM_SIZE;
						ctx->pending_frame_height = STREAM_SIZE;
						ctx->pending_consume_idx = -1;
					}

					// Write to buffer that encode isn't reading
					int write_idx = (ctx->pending_consume_idx == 0) ? 1 : 0;
					memcpy(ctx->pending_frame[write_idx], video_data, data_size);
					ctx->pending_frame_linesize = video_linesize;
					ctx->pending_produce_idx = write_idx;
					ctx->pending_frame_ready = true;
					pthread_cond_signal(&ctx->frame_cond);

					pthread_mutex_unlock(&ctx->mutex);
					gs_stagesurface_unmap(ctx->crop_stagesurface);
				}
			}
		} // end of else (standard path)
	}

	gs_texture_t *output = tex;

	// Render decoded frame with double-buffer
	pthread_mutex_lock(&ctx->mutex);

	bool has_decoded_frame = ctx->decoded_frame_ready;
	bool is_nv12 = ctx->decoded_frame_is_nv12;
	uint32_t w = ctx->decoded_frame_width;
	uint32_t h = ctx->decoded_frame_height;
	int read_idx = -1;

	if (has_decoded_frame) {
		// Take ownership of the buffer
		ctx->decode_consume_idx = ctx->decode_produce_idx;
		read_idx = ctx->decode_consume_idx;
		ctx->decoded_frame_ready = false;
	}

	pthread_mutex_unlock(&ctx->mutex);

	// Process outside mutex - WHEP can write to other buffer now
	if (has_decoded_frame && read_idx >= 0) {
		if (is_nv12 && ctx->nv12_y_data[read_idx] && ctx->nv12_uv_data[read_idx]) {
			// Create Y texture
			if (!ctx->nv12_tex_y || gs_texture_get_width(ctx->nv12_tex_y) != w ||
			    gs_texture_get_height(ctx->nv12_tex_y) != h) {
				if (ctx->nv12_tex_y)
					gs_texture_destroy(ctx->nv12_tex_y);
				ctx->nv12_tex_y = gs_texture_create(w, h, GS_R8, 1, NULL, GS_DYNAMIC);
			}

			// Create UV texture
			if (!ctx->nv12_tex_uv || gs_texture_get_width(ctx->nv12_tex_uv) != w / 2 ||
			    gs_texture_get_height(ctx->nv12_tex_uv) != h / 2) {
				if (ctx->nv12_tex_uv)
					gs_texture_destroy(ctx->nv12_tex_uv);
				ctx->nv12_tex_uv = gs_texture_create(w / 2, h / 2, GS_R8G8, 1, NULL, GS_DYNAMIC);
			}

			// Create texrender
			if (!ctx->nv12_texrender)
				ctx->nv12_texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

			// Load effect
			if (!ctx->nv12_effect) {
				char *effect_path = obs_module_file("nv12_to_rgb.effect");
				if (effect_path) {
					ctx->nv12_effect = gs_effect_create_from_file(effect_path, NULL);
					bfree(effect_path);
				}
			}

			// Upload textures from double-buffer
			if (ctx->nv12_tex_y && ctx->nv12_tex_uv) {
				gs_texture_set_image(ctx->nv12_tex_y, ctx->nv12_y_data[read_idx], ctx->nv12_y_linesize,
						     false);
				gs_texture_set_image(ctx->nv12_tex_uv, ctx->nv12_uv_data[read_idx],
						     ctx->nv12_uv_linesize, false);
			}

			// Render NV12 to RGB
			if (ctx->nv12_effect && ctx->nv12_tex_y && ctx->nv12_tex_uv && ctx->nv12_texrender) {
				gs_texrender_reset(ctx->nv12_texrender);
				if (gs_texrender_begin(ctx->nv12_texrender, w, h)) {
					struct vec4 clear_color;
					vec4_zero(&clear_color);
					gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
					gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

					gs_eparam_t *param_y = gs_effect_get_param_by_name(ctx->nv12_effect, "image");
					gs_eparam_t *param_uv =
						gs_effect_get_param_by_name(ctx->nv12_effect, "image_uv");

					if (param_y && param_uv) {
						gs_effect_set_texture(param_y, ctx->nv12_tex_y);
						gs_effect_set_texture(param_uv, ctx->nv12_tex_uv);

						gs_technique_t *tech =
							gs_effect_get_technique(ctx->nv12_effect, "Draw");
						gs_technique_begin(tech);
						gs_technique_begin_pass(tech, 0);
						gs_draw_sprite(ctx->nv12_tex_y, 0, w, h);
						gs_technique_end_pass(tech);
						gs_technique_end(tech);
					}

					gs_texrender_end(ctx->nv12_texrender);
				}

				gs_texture_t *rgb_tex = gs_texrender_get_texture(ctx->nv12_texrender);
				if (rgb_tex)
					output = rgb_tex;
			}
		} else if (ctx->decoded_frame[read_idx]) {
			if (!ctx->output_texture || gs_texture_get_width(ctx->output_texture) != w ||
			    gs_texture_get_height(ctx->output_texture) != h) {
				if (ctx->output_texture)
					gs_texture_destroy(ctx->output_texture);
				ctx->output_texture = gs_texture_create(w, h, GS_BGRA, 1, NULL, GS_DYNAMIC);
			}

			if (ctx->output_texture) {
				gs_texture_set_image(ctx->output_texture, ctx->decoded_frame[read_idx], w * 4, false);
				output = ctx->output_texture;
			}
		}

		// Release buffer ownership
		pthread_mutex_lock(&ctx->mutex);
		ctx->decode_consume_idx = -1;
		pthread_mutex_unlock(&ctx->mutex);
	}

	// Final render
	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(default_effect, "Draw");

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	if (ctx->streaming && output != tex) {
		float scale = (parent_width < parent_height) ? (float)STREAM_SIZE / (float)parent_width
							     : (float)STREAM_SIZE / (float)parent_height;
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
	UNUSED_PARAMETER(success);
	UNUSED_PARAMETER(error);
	obs_source_update_properties(ctx->source);
}

static bool on_login_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	if (daydream_auth_is_logged_in(ctx->auth))
		return false;

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

	if (ctx->stopping || !result.success) {
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

	struct daydream_encoder_config enc_config = {
		.width = STREAM_SIZE,
		.height = STREAM_SIZE,
		.fps = target_fps,
		.bitrate = 500000,
#if defined(__APPLE__)
		// Zero-copy requires Metal backend (OBS 31+), disabled for now due to OpenGL render target issues
		.use_zerocopy = false,
#endif
	};
	ctx->encoder = daydream_encoder_create(&enc_config);
	if (!ctx->encoder) {
		bfree(api_key_copy);
		daydream_api_free_result(&result);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

#if defined(__APPLE__)
	// Mark that we want to use zero-copy, texture will be created in render thread
	if (daydream_encoder_is_zerocopy(ctx->encoder)) {
		ctx->use_zerocopy = true;
		blog(LOG_INFO, "[Daydream] Zero-copy encoding requested, texture will be created in render thread");
	}
#endif

	struct daydream_decoder_config dec_config = {
		.width = STREAM_SIZE,
		.height = STREAM_SIZE,
	};
	ctx->decoder = daydream_decoder_create(&dec_config);
	if (!ctx->decoder) {
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

	// Initialize adaptive bitrate control
	ctx->current_bitrate = 500000; // Start at 500kbps
	ctx->min_bitrate = 200000;     // Min 200kbps
	ctx->max_bitrate = 2000000;    // Max 2Mbps
	ctx->last_rtt_ms = -1;
	ctx->avg_rtt_ms = -1;
	ctx->last_bitrate_check = os_gettime_ns();

	ctx->encode_thread_running = true;
	pthread_create(&ctx->encode_thread, NULL, encode_thread_func, ctx);

	const char *whep_url = daydream_whip_get_whep_url(ctx->whip);
	if (whep_url) {
		ctx->whep_url = bstrdup(whep_url);

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
	}

	bfree(api_key_copy);
	daydream_api_free_result(&result);
	ctx->start_thread_running = false;
	pthread_mutex_unlock(&ctx->mutex);

	return NULL;
}

static bool on_start_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->streaming || ctx->start_thread_running) {
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	const char *api_key = daydream_auth_get_api_key(ctx->auth);
	if (!api_key || strlen(api_key) == 0) {
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	ctx->start_thread_running = true;
	ctx->stopping = false;
	pthread_create(&ctx->start_thread, NULL, start_streaming_thread_func, ctx);

	pthread_mutex_unlock(&ctx->mutex);
	return false;
}

static bool on_stop_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_filter *ctx = data;

	pthread_mutex_lock(&ctx->mutex);
	if (!ctx->streaming) {
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
