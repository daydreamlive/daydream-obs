#include "daydream-filter.h"
#include "daydream-api.h"
#include "daydream-auth.h"
#include "daydream-debounce.h"
#include "daydream-encoder.h"
#include "daydream-framebuffer.h"
#include "daydream-decoder.h"
#include "daydream-frameskip.h"
#include "daydream-stream.h"
#include "daydream-whip.h"
#include "daydream-whep.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <limits.h>
#include <inttypes.h>
#include <string.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#define PROP_LOGIN "login"
#define PROP_LOGOUT "logout"
#define PROP_LOGIN_STATUS "login_status"
#define PROP_MODEL "model"
#define PROP_NEGATIVE_PROMPT "negative_prompt"
#define PROP_GUIDANCE "guidance"
#define PROP_DELTA "delta"
#define PROP_NUM_STEPS "num_steps"
#define PROP_ADD_NOISE "add_noise"

// Prompt Schedule
#define PROP_PROMPT_COUNT "prompt_count"
#define PROP_PROMPT_1 "prompt_1"
#define PROP_PROMPT_1_WEIGHT "prompt_1_weight"
#define PROP_PROMPT_2 "prompt_2"
#define PROP_PROMPT_2_WEIGHT "prompt_2_weight"
#define PROP_PROMPT_3 "prompt_3"
#define PROP_PROMPT_3_WEIGHT "prompt_3_weight"
#define PROP_PROMPT_4 "prompt_4"
#define PROP_PROMPT_4_WEIGHT "prompt_4_weight"
#define PROP_PROMPT_INTERP "prompt_interpolation"
#define PROP_NORMALIZE_PROMPT "normalize_prompt_weights"

// Seed Schedule
#define PROP_SEED_COUNT "seed_count"
#define PROP_SEED_1 "seed_1"
#define PROP_SEED_1_WEIGHT "seed_1_weight"
#define PROP_SEED_2 "seed_2"
#define PROP_SEED_2_WEIGHT "seed_2_weight"
#define PROP_SEED_3 "seed_3"
#define PROP_SEED_3_WEIGHT "seed_3_weight"
#define PROP_SEED_4 "seed_4"
#define PROP_SEED_4_WEIGHT "seed_4_weight"
#define PROP_SEED_INTERP "seed_interpolation"
#define PROP_NORMALIZE_SEED "normalize_seed_weights"

// Step Schedule (t_index_list)
#define PROP_STEP_COUNT "step_count"
#define PROP_STEP_1 "step_1"
#define PROP_STEP_2 "step_2"
#define PROP_STEP_3 "step_3"
#define PROP_STEP_4 "step_4"

// IP Adapter
#define PROP_IP_ADAPTER_ENABLED "ip_adapter_enabled"
#define PROP_IP_ADAPTER_SCALE "ip_adapter_scale"
#define PROP_IP_ADAPTER_TYPE "ip_adapter_type"
#define PROP_STYLE_IMAGE_URL "style_image_url"

// ControlNet
#define PROP_DEPTH_SCALE "depth_scale"
#define PROP_CANNY_SCALE "canny_scale"
#define PROP_TILE_SCALE "tile_scale"
#define PROP_OPENPOSE_SCALE "openpose_scale"
#define PROP_HED_SCALE "hed_scale"
#define PROP_COLOR_SCALE "color_scale"

#define PROP_START "start"
#define PROP_STOP "stop"

// Experimental
#define PROP_FRAME_SKIP_ENABLED "frame_skip_enabled"
#define PROP_BLUR_SIZE "blur_size"

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

	char *negative_prompt;
	char *model;
	float guidance;
	float delta;
	int num_inference_steps;
	bool add_noise;

	// Prompt Schedule
	int prompt_count;
	char *prompts[DAYDREAM_MAX_SCHEDULE_SLOTS];
	float prompt_weights[DAYDREAM_MAX_SCHEDULE_SLOTS];
	char *prompt_interpolation;
	bool normalize_prompt_weights;

	// Seed Schedule
	int seed_count;
	int seeds[DAYDREAM_MAX_SCHEDULE_SLOTS];
	float seed_weights[DAYDREAM_MAX_SCHEDULE_SLOTS];
	char *seed_interpolation;
	bool normalize_seed_weights;

	// Step Schedule (t_index_list)
	int step_count;
	int step_indices[DAYDREAM_MAX_SCHEDULE_SLOTS];

	// IP Adapter
	bool ip_adapter_enabled;
	float ip_adapter_scale;
	char *ip_adapter_type;
	char *style_image_url;

	// ControlNet scales
	float depth_scale;
	float canny_scale;
	float tile_scale;
	float openpose_scale;
	float hed_scale;
	float color_scale;

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

	// Encode buffer (extracted module)
	struct daydream_framebuffer *encode_buffer;

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

	// Blur background for letterboxing
	gs_texrender_t *blur_texrender;
	gs_texrender_t *blur_texrender2;
	gs_effect_t *blur_effect;

	pthread_mutex_t mutex;

	uint64_t frame_count;
	uint64_t last_encode_time;
	uint32_t target_fps;

	// Parameter update tracking (uses extracted debounce module)
	struct daydream_debounce *debounce;
	pthread_t update_thread;
	pthread_cond_t update_cond;
	bool update_thread_running;

	// Frame skip filter (extracted module)
	struct daydream_frameskip *frameskip;

	// Stream orchestrator (extracted module)
	struct daydream_stream *stream;

	// Experimental: Blur background
	int blur_size;

	// Debug: track first render
	bool first_frame_rendered;
};

// Forward declaration
static void schedule_params_update(struct daydream_filter *ctx, uint64_t flags);

static const char *daydream_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Daydream";
}

// Use daydream_str_changed from debounce module for string comparison

static void daydream_filter_update(void *data, obs_data_t *settings)
{
	struct daydream_filter *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	bool is_streaming = ctx->streaming;
	uint64_t update_flags = 0;

	// Read new values from settings
	const char *new_negative_prompt = obs_data_get_string(settings, PROP_NEGATIVE_PROMPT);
	const char *new_model = obs_data_get_string(settings, PROP_MODEL);
	float new_guidance = (float)obs_data_get_double(settings, PROP_GUIDANCE);
	float new_delta = (float)obs_data_get_double(settings, PROP_DELTA);
	int new_num_steps = (int)obs_data_get_int(settings, PROP_NUM_STEPS);
	bool new_add_noise = obs_data_get_bool(settings, PROP_ADD_NOISE);

	// Prompt Schedule
	int new_prompt_count = (int)obs_data_get_int(settings, PROP_PROMPT_COUNT);
	const char *new_prompts[DAYDREAM_MAX_SCHEDULE_SLOTS];
	float new_prompt_weights[DAYDREAM_MAX_SCHEDULE_SLOTS];
	new_prompts[0] = obs_data_get_string(settings, PROP_PROMPT_1);
	new_prompt_weights[0] = (float)obs_data_get_double(settings, PROP_PROMPT_1_WEIGHT);
	new_prompts[1] = obs_data_get_string(settings, PROP_PROMPT_2);
	new_prompt_weights[1] = (float)obs_data_get_double(settings, PROP_PROMPT_2_WEIGHT);
	new_prompts[2] = obs_data_get_string(settings, PROP_PROMPT_3);
	new_prompt_weights[2] = (float)obs_data_get_double(settings, PROP_PROMPT_3_WEIGHT);
	new_prompts[3] = obs_data_get_string(settings, PROP_PROMPT_4);
	new_prompt_weights[3] = (float)obs_data_get_double(settings, PROP_PROMPT_4_WEIGHT);
	const char *new_prompt_interp = obs_data_get_string(settings, PROP_PROMPT_INTERP);
	bool new_normalize_prompt = obs_data_get_bool(settings, PROP_NORMALIZE_PROMPT);

	// Seed Schedule
	int new_seed_count = (int)obs_data_get_int(settings, PROP_SEED_COUNT);
	int new_seeds[DAYDREAM_MAX_SCHEDULE_SLOTS];
	float new_seed_weights[DAYDREAM_MAX_SCHEDULE_SLOTS];
	new_seeds[0] = (int)obs_data_get_int(settings, PROP_SEED_1);
	new_seed_weights[0] = (float)obs_data_get_double(settings, PROP_SEED_1_WEIGHT);
	new_seeds[1] = (int)obs_data_get_int(settings, PROP_SEED_2);
	new_seed_weights[1] = (float)obs_data_get_double(settings, PROP_SEED_2_WEIGHT);
	new_seeds[2] = (int)obs_data_get_int(settings, PROP_SEED_3);
	new_seed_weights[2] = (float)obs_data_get_double(settings, PROP_SEED_3_WEIGHT);
	new_seeds[3] = (int)obs_data_get_int(settings, PROP_SEED_4);
	new_seed_weights[3] = (float)obs_data_get_double(settings, PROP_SEED_4_WEIGHT);
	const char *new_seed_interp = obs_data_get_string(settings, PROP_SEED_INTERP);
	bool new_normalize_seed = obs_data_get_bool(settings, PROP_NORMALIZE_SEED);

	// Step Schedule
	int new_step_count = (int)obs_data_get_int(settings, PROP_STEP_COUNT);
	int new_step_indices[DAYDREAM_MAX_SCHEDULE_SLOTS];
	new_step_indices[0] = (int)obs_data_get_int(settings, PROP_STEP_1);
	new_step_indices[1] = (int)obs_data_get_int(settings, PROP_STEP_2);
	new_step_indices[2] = (int)obs_data_get_int(settings, PROP_STEP_3);
	new_step_indices[3] = (int)obs_data_get_int(settings, PROP_STEP_4);

	// IP Adapter
	bool new_ip_enabled = obs_data_get_bool(settings, PROP_IP_ADAPTER_ENABLED);
	float new_ip_scale = (float)obs_data_get_double(settings, PROP_IP_ADAPTER_SCALE);
	const char *new_ip_type = obs_data_get_string(settings, PROP_IP_ADAPTER_TYPE);
	const char *new_style_url = obs_data_get_string(settings, PROP_STYLE_IMAGE_URL);

	// ControlNet scales
	float new_depth = (float)obs_data_get_double(settings, PROP_DEPTH_SCALE);
	float new_canny = (float)obs_data_get_double(settings, PROP_CANNY_SCALE);
	float new_tile = (float)obs_data_get_double(settings, PROP_TILE_SCALE);
	float new_openpose = (float)obs_data_get_double(settings, PROP_OPENPOSE_SCALE);
	float new_hed = (float)obs_data_get_double(settings, PROP_HED_SCALE);
	float new_color = (float)obs_data_get_double(settings, PROP_COLOR_SCALE);

	// Experimental
	bool new_frame_skip = obs_data_get_bool(settings, PROP_FRAME_SKIP_ENABLED);
	int new_blur_size = (int)obs_data_get_int(settings, PROP_BLUR_SIZE);

	// Detect changes if streaming
	if (is_streaming) {
		// Prompt changes
		if (new_prompt_count != ctx->prompt_count)
			update_flags |= UPDATE_FLAG_PROMPT;
		for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS && !(update_flags & UPDATE_FLAG_PROMPT); i++) {
			if (daydream_str_changed(ctx->prompts[i], new_prompts[i]) ||
			    ctx->prompt_weights[i] != new_prompt_weights[i])
				update_flags |= UPDATE_FLAG_PROMPT;
		}

		if (daydream_str_changed(ctx->negative_prompt, new_negative_prompt))
			update_flags |= UPDATE_FLAG_NEGATIVE_PROMPT;

		// Seed changes
		if (new_seed_count != ctx->seed_count)
			update_flags |= UPDATE_FLAG_SEED;
		for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS && !(update_flags & UPDATE_FLAG_SEED); i++) {
			if (ctx->seeds[i] != new_seeds[i] || ctx->seed_weights[i] != new_seed_weights[i])
				update_flags |= UPDATE_FLAG_SEED;
		}

		// Step schedule changes
		if (new_step_count != ctx->step_count)
			update_flags |= UPDATE_FLAG_STEP_SCHEDULE;
		for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS && !(update_flags & UPDATE_FLAG_STEP_SCHEDULE); i++) {
			if (ctx->step_indices[i] != new_step_indices[i])
				update_flags |= UPDATE_FLAG_STEP_SCHEDULE;
		}

		if (ctx->guidance != new_guidance)
			update_flags |= UPDATE_FLAG_GUIDANCE;
		if (ctx->delta != new_delta)
			update_flags |= UPDATE_FLAG_DELTA;

		// ControlNet changes
		if (ctx->depth_scale != new_depth || ctx->canny_scale != new_canny || ctx->tile_scale != new_tile ||
		    ctx->openpose_scale != new_openpose || ctx->hed_scale != new_hed || ctx->color_scale != new_color)
			update_flags |= UPDATE_FLAG_CONTROLNETS;

		// IP Adapter changes
		if (ctx->ip_adapter_enabled != new_ip_enabled || ctx->ip_adapter_scale != new_ip_scale ||
		    daydream_str_changed(ctx->style_image_url, new_style_url))
			update_flags |= UPDATE_FLAG_IP_ADAPTER;

		// Interpolation changes
		if (daydream_str_changed(ctx->prompt_interpolation, new_prompt_interp) ||
		    ctx->normalize_prompt_weights != new_normalize_prompt ||
		    daydream_str_changed(ctx->seed_interpolation, new_seed_interp) ||
		    ctx->normalize_seed_weights != new_normalize_seed)
			update_flags |= UPDATE_FLAG_INTERP;
	}

	// Now update context with new values
	bfree(ctx->negative_prompt);
	bfree(ctx->model);
	bfree(ctx->ip_adapter_type);
	bfree(ctx->style_image_url);
	bfree(ctx->prompt_interpolation);
	bfree(ctx->seed_interpolation);
	for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		bfree(ctx->prompts[i]);
	}

	ctx->negative_prompt = bstrdup(new_negative_prompt);
	ctx->model = bstrdup(new_model);
	ctx->guidance = new_guidance;
	ctx->delta = new_delta;
	ctx->num_inference_steps = new_num_steps;
	ctx->add_noise = new_add_noise;

	ctx->prompt_count = new_prompt_count;
	for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		ctx->prompts[i] = bstrdup(new_prompts[i]);
		ctx->prompt_weights[i] = new_prompt_weights[i];
	}
	ctx->prompt_interpolation = bstrdup(new_prompt_interp);
	ctx->normalize_prompt_weights = new_normalize_prompt;

	ctx->seed_count = new_seed_count;
	for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		ctx->seeds[i] = new_seeds[i];
		ctx->seed_weights[i] = new_seed_weights[i];
	}
	ctx->seed_interpolation = bstrdup(new_seed_interp);
	ctx->normalize_seed_weights = new_normalize_seed;

	ctx->step_count = new_step_count;
	for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		ctx->step_indices[i] = new_step_indices[i];
	}

	ctx->ip_adapter_enabled = new_ip_enabled;
	ctx->ip_adapter_scale = new_ip_scale;
	ctx->ip_adapter_type = bstrdup(new_ip_type);
	ctx->style_image_url = bstrdup(new_style_url);

	ctx->depth_scale = new_depth;
	ctx->canny_scale = new_canny;
	ctx->tile_scale = new_tile;
	ctx->openpose_scale = new_openpose;
	ctx->hed_scale = new_hed;
	ctx->color_scale = new_color;

	daydream_frameskip_set_enabled(ctx->frameskip, new_frame_skip);
	ctx->blur_size = new_blur_size;

	pthread_mutex_unlock(&ctx->mutex);

	// Schedule parameter update if any hot params changed during streaming
	if (update_flags != 0) {
		schedule_params_update(ctx, update_flags);
	}
}

#define PARAMS_UPDATE_DELAY_NS (100 * 1000000ULL) // 100ms debounce
#define PARAMS_UPDATE_POLL_MS 10                  // Poll interval while waiting for debounce

static void *update_thread_func(void *arg)
{
	struct daydream_filter *ctx = arg;

	while (ctx->update_thread_running) {
		pthread_mutex_lock(&ctx->mutex);

		// Wait for update signal (debounce module has pending flags)
		while (ctx->update_thread_running && !daydream_debounce_has_pending(ctx->debounce)) {
			pthread_cond_wait(&ctx->update_cond, &ctx->mutex);
		}

		if (!ctx->update_thread_running) {
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}

		// Wait for debounce delay to pass (poll with sleep)
		while (!daydream_debounce_ready(ctx->debounce) && ctx->update_thread_running) {
			pthread_mutex_unlock(&ctx->mutex);
			os_sleep_ms(PARAMS_UPDATE_POLL_MS);
			pthread_mutex_lock(&ctx->mutex);
		}

		// Check if still valid to send
		if (!ctx->update_thread_running || !ctx->streaming) {
			pthread_mutex_unlock(&ctx->mutex);
			continue;
		}

		// Consume flags from debounce module
		uint64_t flags = daydream_debounce_consume(ctx->debounce);
		if (flags == 0) {
			pthread_mutex_unlock(&ctx->mutex);
			continue;
		}

		// Build params struct from current state
		struct daydream_stream_params params = {0};
		params.model_id = ctx->model;
		params.negative_prompt = ctx->negative_prompt;
		params.guidance = ctx->guidance;
		params.delta = ctx->delta;
		params.num_inference_steps = ctx->num_inference_steps;

		params.prompt_schedule.count = ctx->prompt_count;
		for (int i = 0; i < ctx->prompt_count; i++) {
			params.prompt_schedule.prompts[i] = ctx->prompts[i];
			params.prompt_schedule.weights[i] = ctx->prompt_weights[i];
		}

		params.seed_schedule.count = ctx->seed_count;
		for (int i = 0; i < ctx->seed_count; i++) {
			params.seed_schedule.seeds[i] = ctx->seeds[i];
			params.seed_schedule.weights[i] = ctx->seed_weights[i];
		}

		params.step_schedule.count = ctx->step_count;
		for (int i = 0; i < ctx->step_count; i++) {
			params.step_schedule.steps[i] = ctx->step_indices[i];
		}

		params.ip_adapter.enabled = ctx->ip_adapter_enabled;
		params.ip_adapter.scale = ctx->ip_adapter_scale;
		params.ip_adapter.type = ctx->ip_adapter_type;
		params.ip_adapter.style_image_url = ctx->style_image_url;

		params.controlnets.depth_scale = ctx->depth_scale;
		params.controlnets.canny_scale = ctx->canny_scale;
		params.controlnets.tile_scale = ctx->tile_scale;
		params.controlnets.openpose_scale = ctx->openpose_scale;
		params.controlnets.hed_scale = ctx->hed_scale;
		params.controlnets.color_scale = ctx->color_scale;

		params.prompt_interpolation_method = ctx->prompt_interpolation;
		params.normalize_prompt_weights = ctx->normalize_prompt_weights;
		params.seed_interpolation_method = ctx->seed_interpolation;
		params.normalize_seed_weights = ctx->normalize_seed_weights;

		char *stream_id = bstrdup(ctx->stream_id);
		char *api_key = daydream_auth_get_api_key(ctx->auth);

		pthread_mutex_unlock(&ctx->mutex);

		// Send PATCH request
		if (stream_id && api_key) {
			daydream_error_t err = daydream_api_update_stream(api_key, stream_id, &params, flags);
			if (err != DAYDREAM_OK) {
				blog(LOG_WARNING, "[Daydream] Failed to update stream parameters: %s",
				     daydream_error_string(err));
			}
		}

		bfree(api_key);
		bfree(stream_id);
	}

	return NULL;
}

static void schedule_params_update(struct daydream_filter *ctx, uint64_t flags)
{
	pthread_mutex_lock(&ctx->mutex);
	daydream_debounce_schedule(ctx->debounce, flags);
	pthread_cond_signal(&ctx->update_cond);
	pthread_mutex_unlock(&ctx->mutex);
}

static void on_whep_frame(const uint8_t *data, size_t size, uint32_t rtp_timestamp, bool is_keyframe, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(is_keyframe);

	if (!ctx || !ctx->decoder || ctx->stopping)
		return;

	// Frame skip: drop out-of-order frames using extracted module
	if (!daydream_frameskip_should_display(ctx->frameskip, rtp_timestamp)) {
		struct daydream_frameskip_stats stats;
		daydream_frameskip_get_stats(ctx->frameskip, &stats);
		if (stats.frames_skipped % 100 == 1) {
			blog(LOG_INFO,
			     "[Daydream] Skipped out-of-order frame: rtp=%u, last=%u, skipped %" PRIu64 "/%" PRIu64,
			     rtp_timestamp, stats.last_rtp_timestamp, stats.frames_skipped, stats.frames_received);
		}
		return;
	}

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

// Stream adapter callbacks for daydream_stream module
static void stream_api_create(const char *api_key, uint32_t width, uint32_t height, void *userdata,
			      struct daydream_stream_api_result *result)
{
	struct daydream_filter *ctx = userdata;

	// Build full params from filter context
	pthread_mutex_lock(&ctx->mutex);

	struct daydream_stream_params params = {
		.model_id = ctx->model,
		.negative_prompt = ctx->negative_prompt,
		.guidance = ctx->guidance,
		.delta = ctx->delta,
		.num_inference_steps = ctx->num_inference_steps,
		.width = (int)width,
		.height = (int)height,
		.do_add_noise = ctx->add_noise,
		.ip_adapter =
			{
				.enabled = ctx->ip_adapter_enabled,
				.scale = ctx->ip_adapter_scale,
				.type = ctx->ip_adapter_type,
				.style_image_url = ctx->style_image_url,
			},
		.prompt_interpolation_method = ctx->prompt_interpolation,
		.normalize_prompt_weights = ctx->normalize_prompt_weights,
		.seed_interpolation_method = ctx->seed_interpolation,
		.normalize_seed_weights = ctx->normalize_seed_weights,
		.controlnets =
			{
				.depth_scale = ctx->depth_scale,
				.canny_scale = ctx->canny_scale,
				.tile_scale = ctx->tile_scale,
				.openpose_scale = ctx->openpose_scale,
				.hed_scale = ctx->hed_scale,
				.color_scale = ctx->color_scale,
			},
	};

	// Copy schedules
	params.prompt_schedule.count = ctx->prompt_count;
	for (int i = 0; i < ctx->prompt_count && i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		params.prompt_schedule.prompts[i] = ctx->prompts[i];
		params.prompt_schedule.weights[i] = ctx->prompt_weights[i];
	}

	params.seed_schedule.count = ctx->seed_count;
	for (int i = 0; i < ctx->seed_count && i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		params.seed_schedule.seeds[i] = ctx->seeds[i];
		params.seed_schedule.weights[i] = ctx->seed_weights[i];
	}

	params.step_schedule.count = ctx->step_count;
	for (int i = 0; i < ctx->step_count && i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		params.step_schedule.steps[i] = ctx->step_indices[i];
	}

	pthread_mutex_unlock(&ctx->mutex);

	struct daydream_stream_result api_result = daydream_api_create_stream(api_key, &params);

	if (api_result.error == DAYDREAM_OK) {
		result->success = true;
		result->stream_id = api_result.stream_id ? strdup(api_result.stream_id) : NULL;
		result->whip_url = api_result.whip_url ? strdup(api_result.whip_url) : NULL;

		// Store stream_id in filter for update API calls
		pthread_mutex_lock(&ctx->mutex);
		bfree(ctx->stream_id);
		ctx->stream_id = bstrdup(api_result.stream_id);
		bfree(ctx->whip_url);
		ctx->whip_url = bstrdup(api_result.whip_url);
		pthread_mutex_unlock(&ctx->mutex);
	} else {
		result->success = false;
		result->error_message = strdup(api_result.error_detail ? api_result.error_detail
								       : daydream_error_string(api_result.error));
	}

	daydream_api_free_result(&api_result);
}

static void *stream_encoder_create(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, void *userdata)
{
	struct daydream_filter *ctx = userdata;

	struct daydream_encoder_config config = {
		.width = width,
		.height = height,
		.fps = fps,
		.bitrate = bitrate,
#if defined(__APPLE__)
		.use_zerocopy = false,
#endif
	};

	ctx->encoder = daydream_encoder_create(&config);

#if defined(__APPLE__)
	if (ctx->encoder && daydream_encoder_is_zerocopy(ctx->encoder)) {
		ctx->use_zerocopy = true;
	}
#endif

	return ctx->encoder;
}

static void stream_encoder_destroy(void *encoder, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(encoder);

	if (ctx->encoder) {
		daydream_encoder_destroy(ctx->encoder);
		ctx->encoder = NULL;
	}
#if defined(__APPLE__)
	ctx->use_zerocopy = false;
#endif
}

static void *stream_decoder_create(uint32_t width, uint32_t height, void *userdata)
{
	struct daydream_filter *ctx = userdata;

	struct daydream_decoder_config config = {
		.width = width,
		.height = height,
	};

	ctx->decoder = daydream_decoder_create(&config);
	return ctx->decoder;
}

static void stream_decoder_destroy(void *decoder, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(decoder);

	if (ctx->decoder) {
		daydream_decoder_destroy(ctx->decoder);
		ctx->decoder = NULL;
	}
}

static void *stream_whip_create(const char *url, const char *api_key, void *userdata)
{
	struct daydream_filter *ctx = userdata;

	struct daydream_whip_config config = {
		.whip_url = url,
		.api_key = api_key,
		.width = 512,
		.height = 512,
		.fps = ctx->target_fps,
		.on_state = on_whip_state,
		.userdata = ctx,
	};

	ctx->whip = daydream_whip_create(&config);
	return ctx->whip;
}

static bool stream_whip_connect(void *whip, void *userdata)
{
	UNUSED_PARAMETER(userdata);
	return daydream_whip_connect(whip);
}

static const char *stream_whip_get_whep_url(void *whip, void *userdata)
{
	UNUSED_PARAMETER(userdata);
	return daydream_whip_get_whep_url(whip);
}

static void stream_whip_disconnect(void *whip, void *userdata)
{
	UNUSED_PARAMETER(userdata);
	daydream_whip_disconnect(whip);
}

static void stream_whip_destroy(void *whip, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(whip);

	if (ctx->whip) {
		daydream_whip_destroy(ctx->whip);
		ctx->whip = NULL;
	}
}

static void *stream_whep_create(const char *url, void *userdata)
{
	struct daydream_filter *ctx = userdata;

	pthread_mutex_lock(&ctx->mutex);
	bfree(ctx->whep_url);
	ctx->whep_url = bstrdup(url);
	pthread_mutex_unlock(&ctx->mutex);

	struct daydream_whep_config config = {
		.whep_url = url,
		.api_key = NULL,
		.on_frame = on_whep_frame,
		.on_state = on_whep_state,
		.userdata = ctx,
	};

	ctx->whep = daydream_whep_create(&config);
	return ctx->whep;
}

static bool stream_whep_connect(void *whep, void *userdata)
{
	UNUSED_PARAMETER(userdata);
	return daydream_whep_connect(whep);
}

static void stream_whep_disconnect(void *whep, void *userdata)
{
	UNUSED_PARAMETER(userdata);
	daydream_whep_disconnect(whep);
}

static void stream_whep_destroy(void *whep, void *userdata)
{
	struct daydream_filter *ctx = userdata;
	UNUSED_PARAMETER(whep);

	if (ctx->whep) {
		daydream_whep_destroy(ctx->whep);
		ctx->whep = NULL;
	}
}

static void stream_on_state_change(daydream_stream_state_t state, const char *error, void *userdata)
{
	UNUSED_PARAMETER(userdata);

	if (state == DAYDREAM_STREAM_RUNNING) {
		blog(LOG_INFO, "[Daydream] Stream started successfully");
	} else if (state == DAYDREAM_STREAM_ERROR) {
		blog(LOG_ERROR, "[Daydream] Stream error: %s", error ? error : "unknown");
	} else if (state == DAYDREAM_STREAM_IDLE) {
		blog(LOG_INFO, "[Daydream] Stream stopped");
	}
}

static daydream_stream_ops_t create_stream_ops(struct daydream_filter *ctx)
{
	return (daydream_stream_ops_t){
		.api_create_stream = stream_api_create,
		.encoder_create = stream_encoder_create,
		.encoder_destroy = stream_encoder_destroy,
		.decoder_create = stream_decoder_create,
		.decoder_destroy = stream_decoder_destroy,
		.whip_create = stream_whip_create,
		.whip_connect = stream_whip_connect,
		.whip_get_whep_url = stream_whip_get_whep_url,
		.whip_disconnect = stream_whip_disconnect,
		.whip_destroy = stream_whip_destroy,
		.whep_create = stream_whep_create,
		.whep_connect = stream_whep_connect,
		.whep_disconnect = stream_whep_disconnect,
		.whep_destroy = stream_whep_destroy,
		.on_state_change = stream_on_state_change,
		.userdata = ctx,
	};
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
		// Wait for frame using framebuffer's internal synchronization
		// Use 100ms timeout to periodically check stopping flag
		while (ctx->encode_thread_running && !ctx->stopping) {
			if (daydream_framebuffer_wait(ctx->encode_buffer, 100)) {
				break; // Frame available
			}
		}

		if (!ctx->encode_thread_running || ctx->stopping) {
			break;
		}

#if defined(__APPLE__)
		bool zerocopy = ctx->use_zerocopy;
#else
		bool zerocopy = false;
#endif
		uint8_t *frame_data = NULL;
		uint32_t frame_linesize = 0;
		uint32_t frame_width = 0;
		uint32_t frame_height = 0;

		// Get frame from buffer (for non-zerocopy) or just consume the ready flag (for zerocopy)
		if (!daydream_framebuffer_begin_read(ctx->encode_buffer, &frame_data, &frame_width, &frame_height,
						     &frame_linesize)) {
			continue; // No frame available
		}

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
		}

		// Release buffer ownership
		daydream_framebuffer_end_read(ctx->encode_buffer);

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
	ctx->decode_consume_idx = -1;

	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->update_cond, NULL);

	ctx->auth = daydream_auth_create();
	ctx->frameskip = daydream_frameskip_create(false); // Disabled by default
	ctx->debounce = daydream_debounce_create(PARAMS_UPDATE_DELAY_NS, NULL);
	ctx->encode_buffer = daydream_framebuffer_create();
	daydream_filter_update(ctx, settings);

	return ctx;
}

static void stop_streaming(struct daydream_filter *ctx)
{
	ctx->stopping = true;

	// Stop update thread first
	if (ctx->update_thread_running) {
		ctx->update_thread_running = false;
		pthread_cond_signal(&ctx->update_cond);
		pthread_join(ctx->update_thread, NULL);
	}

	if (ctx->encode_thread_running) {
		ctx->encode_thread_running = false;
		daydream_framebuffer_signal(ctx->encode_buffer);
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

	// Handle WHEP cleanup separately (not managed by stream module)
	if (ctx->whep) {
		daydream_whep_disconnect(ctx->whep);
		daydream_whep_destroy(ctx->whep);
		ctx->whep = NULL;
	}

	// Use stream module for component cleanup (API, encoder, decoder, WHIP)
	if (ctx->stream) {
		daydream_stream_stop(ctx->stream);
	}

#if defined(__APPLE__)
	// Handle macOS-specific resources not managed by stream module
	if (ctx->iosurface_texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->iosurface_texture);
		obs_leave_graphics();
		ctx->iosurface_texture = NULL;
	}
#endif

	ctx->streaming = false;
	ctx->stopping = false;
	ctx->decoded_frame_ready = false;

	// Clear pending updates using debounce module
	daydream_debounce_reset(ctx->debounce);

	// Trigger UI refresh to re-enable cold parameters
	obs_source_update_properties(ctx->source);
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
	if (ctx->blur_texrender)
		gs_texrender_destroy(ctx->blur_texrender);
	if (ctx->blur_texrender2)
		gs_texrender_destroy(ctx->blur_texrender2);
	if (ctx->blur_effect)
		gs_effect_destroy(ctx->blur_effect);
	obs_leave_graphics();

	daydream_auth_destroy(ctx->auth);
	daydream_frameskip_destroy(ctx->frameskip);
	daydream_debounce_destroy(ctx->debounce);
	daydream_stream_destroy(ctx->stream);

	bfree(ctx->negative_prompt);
	bfree(ctx->model);
	bfree(ctx->ip_adapter_type);
	bfree(ctx->style_image_url);
	bfree(ctx->prompt_interpolation);
	bfree(ctx->seed_interpolation);
	for (int i = 0; i < DAYDREAM_MAX_SCHEDULE_SLOTS; i++) {
		bfree(ctx->prompts[i]);
	}
	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);
	bfree(ctx->decoded_frame[0]);
	bfree(ctx->decoded_frame[1]);
	bfree(ctx->nv12_y_data[0]);
	bfree(ctx->nv12_y_data[1]);
	bfree(ctx->nv12_uv_data[0]);
	bfree(ctx->nv12_uv_data[1]);

	daydream_framebuffer_destroy(ctx->encode_buffer);
	pthread_cond_destroy(&ctx->update_cond);
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
			// Use framebuffer just for signaling (configure if not already done)
			uint32_t fb_w, fb_h;
			daydream_framebuffer_get_dimensions(ctx->encode_buffer, &fb_w, &fb_h);
			if (fb_w != STREAM_SIZE || fb_h != STREAM_SIZE) {
				daydream_framebuffer_configure(ctx->encode_buffer, STREAM_SIZE, STREAM_SIZE, 4);
			}
			daydream_framebuffer_begin_write(ctx->encode_buffer, NULL);
			daydream_framebuffer_end_write(ctx->encode_buffer);
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
					// Configure framebuffer if dimensions changed
					uint32_t fb_w, fb_h;
					daydream_framebuffer_get_dimensions(ctx->encode_buffer, &fb_w, &fb_h);
					if (fb_w != STREAM_SIZE || fb_h != STREAM_SIZE) {
						daydream_framebuffer_configure(ctx->encode_buffer, STREAM_SIZE,
									       STREAM_SIZE, 4);
					}

					// Write frame to encode buffer
					uint32_t fb_linesize;
					uint8_t *write_buf =
						daydream_framebuffer_begin_write(ctx->encode_buffer, &fb_linesize);
					if (write_buf) {
						// Copy row by row to handle linesize differences
						uint32_t copy_size = STREAM_SIZE * 4; // BGRA
						for (uint32_t row = 0; row < STREAM_SIZE; row++) {
							memcpy(write_buf + row * fb_linesize,
							       video_data + row * video_linesize, copy_size);
						}
						daydream_framebuffer_end_write(ctx->encode_buffer);
					}

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
		if (is_nv12 && !ctx->nv12_y_data[read_idx]) {
			blog(LOG_WARNING, "[Daydream] NV12 frame expected but y_data[%d] is NULL", read_idx);
		}
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
			if (!ctx->nv12_effect || !ctx->nv12_tex_y || !ctx->nv12_tex_uv || !ctx->nv12_texrender) {
				blog(LOG_WARNING,
				     "[Daydream] NV12 render prereq failed: effect=%p, tex_y=%p, tex_uv=%p, texrender=%p",
				     (void *)ctx->nv12_effect, (void *)ctx->nv12_tex_y, (void *)ctx->nv12_tex_uv,
				     (void *)ctx->nv12_texrender);
			}
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

						if (!ctx->first_frame_rendered) {
							ctx->first_frame_rendered = true;
							blog(LOG_INFO,
							     "[Daydream] First frame rendered: %ux%u, y_linesize=%u, uv_linesize=%u",
							     w, h, ctx->nv12_y_linesize, ctx->nv12_uv_linesize);
						}
					} else {
						blog(LOG_WARNING,
						     "[Daydream] NV12 render skipped: param_y=%p, param_uv=%p",
						     (void *)param_y, (void *)param_uv);
					}

					gs_texrender_end(ctx->nv12_texrender);
				}
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
			}
		}

		// Release buffer ownership
		pthread_mutex_lock(&ctx->mutex);
		ctx->decode_consume_idx = -1;
		pthread_mutex_unlock(&ctx->mutex);
	}

	// Use cached decoded texture if streaming (regardless of new frame)
	if (ctx->streaming) {
		if (ctx->nv12_texrender) {
			gs_texture_t *rgb_tex = gs_texrender_get_texture(ctx->nv12_texrender);
			if (rgb_tex)
				output = rgb_tex;
		} else if (ctx->output_texture) {
			output = ctx->output_texture;
		}
	}

	// Final render
	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(default_effect, "Draw");

	if (ctx->streaming && output != tex) {
		float scale = (parent_width < parent_height) ? (float)STREAM_SIZE / (float)parent_width
							     : (float)STREAM_SIZE / (float)parent_height;
		float render_size = STREAM_SIZE / scale;
		float render_x = (ctx->width - render_size) / 2.0f;
		float render_y = (ctx->height - render_size) / 2.0f;

		// Simple color blur: downsample then upscale (bilinear filtering does the rest)
		gs_texture_t *blur_tex = NULL;
		int blur_size = ctx->blur_size;
		if (blur_size > 0) {
			if (!ctx->blur_texrender)
				ctx->blur_texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

			if (ctx->blur_texrender) {
				gs_texrender_reset(ctx->blur_texrender);
				if (gs_texrender_begin(ctx->blur_texrender, blur_size, blur_size)) {
					struct vec4 clear_color;
					vec4_zero(&clear_color);
					gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
					gs_ortho(0.0f, (float)blur_size, 0.0f, (float)blur_size, -100.0f, 100.0f);

					gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"),
							      output);
					gs_technique_begin(tech);
					gs_technique_begin_pass(tech, 0);
					gs_draw_sprite(output, 0, blur_size, blur_size);
					gs_technique_end_pass(tech);
					gs_technique_end(tech);

					gs_texrender_end(ctx->blur_texrender);
					blur_tex = gs_texrender_get_texture(ctx->blur_texrender);
				}
			}
		}

		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		// Draw blurred background full screen
		if (blur_tex) {
			gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), blur_tex);
			gs_draw_sprite(blur_tex, 0, ctx->width, ctx->height);
		}

		// Draw actual output centered
		gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), output);
		gs_matrix_push();
		gs_matrix_translate3f(render_x, render_y, 0.0f);
		gs_draw_sprite(output, 0, (uint32_t)render_size, (uint32_t)render_size);
		gs_matrix_pop();

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	} else {
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), output);
		gs_draw_sprite(output, 0, ctx->width, ctx->height);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}
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

static void open_url(const char *url)
{
#if defined(__APPLE__)
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "open \"%s\"", url);
	(void)system(cmd);
#elif defined(_WIN32)
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", url);
	(void)system(cmd);
#else
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "xdg-open \"%s\"", url);
	(void)system(cmd);
#endif
}

static bool on_homepage_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	open_url("https://daydream.live");
	return false;
}

static bool on_github_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	open_url("https://github.com/daydreamlive");
	return false;
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

static bool on_auth_toggle_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct daydream_filter *ctx = data;

	if (daydream_auth_is_logged_in(ctx->auth)) {
		return on_logout_clicked(props, property, data);
	} else {
		return on_login_clicked(props, property, data);
	}
}

static bool on_model_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *model = obs_data_get_string(settings, PROP_MODEL);
	bool is_sd_turbo = model && strcmp(model, "stabilityai/sd-turbo") == 0;

	// SD Turbo: openpose, hed, canny, depth, color
	// Others (SDXL, Dreamshaper, Openjourney): depth, canny, tile

	obs_property_t *depth = obs_properties_get(props, PROP_DEPTH_SCALE);
	obs_property_t *canny = obs_properties_get(props, PROP_CANNY_SCALE);
	obs_property_t *tile = obs_properties_get(props, PROP_TILE_SCALE);
	obs_property_t *openpose = obs_properties_get(props, PROP_OPENPOSE_SCALE);
	obs_property_t *hed = obs_properties_get(props, PROP_HED_SCALE);
	obs_property_t *color = obs_properties_get(props, PROP_COLOR_SCALE);

	if (depth)
		obs_property_set_visible(depth, true);
	if (canny)
		obs_property_set_visible(canny, true);
	if (tile)
		obs_property_set_visible(tile, !is_sd_turbo);
	if (openpose)
		obs_property_set_visible(openpose, is_sd_turbo);
	if (hed)
		obs_property_set_visible(hed, is_sd_turbo);
	if (color)
		obs_property_set_visible(color, is_sd_turbo);

	return true;
}

static bool on_prompt_count_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	int count = (int)obs_data_get_int(settings, PROP_PROMPT_COUNT);

	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_2), count >= 2);
	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_2_WEIGHT), count >= 2);
	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_3), count >= 3);
	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_3_WEIGHT), count >= 3);
	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_4), count >= 4);
	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_4_WEIGHT), count >= 4);

	// Show interpolation options only if multiple prompts
	obs_property_set_visible(obs_properties_get(props, PROP_PROMPT_INTERP), count > 1);
	obs_property_set_visible(obs_properties_get(props, PROP_NORMALIZE_PROMPT), count > 1);

	return true;
}

static bool on_seed_count_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	int count = (int)obs_data_get_int(settings, PROP_SEED_COUNT);

	obs_property_set_visible(obs_properties_get(props, PROP_SEED_2), count >= 2);
	obs_property_set_visible(obs_properties_get(props, PROP_SEED_2_WEIGHT), count >= 2);
	obs_property_set_visible(obs_properties_get(props, PROP_SEED_3), count >= 3);
	obs_property_set_visible(obs_properties_get(props, PROP_SEED_3_WEIGHT), count >= 3);
	obs_property_set_visible(obs_properties_get(props, PROP_SEED_4), count >= 4);
	obs_property_set_visible(obs_properties_get(props, PROP_SEED_4_WEIGHT), count >= 4);

	// Show interpolation options only if multiple seeds
	obs_property_set_visible(obs_properties_get(props, PROP_SEED_INTERP), count > 1);
	obs_property_set_visible(obs_properties_get(props, PROP_NORMALIZE_SEED), count > 1);

	return true;
}

static bool on_step_count_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	int count = (int)obs_data_get_int(settings, PROP_STEP_COUNT);

	obs_property_set_visible(obs_properties_get(props, PROP_STEP_2), count >= 2);
	obs_property_set_visible(obs_properties_get(props, PROP_STEP_3), count >= 3);
	obs_property_set_visible(obs_properties_get(props, PROP_STEP_4), count >= 4);

	return true;
}

static void *start_streaming_thread_func(void *data)
{
	struct daydream_filter *ctx = data;
	const uint32_t STREAM_SIZE = 512;

	// Get API key
	pthread_mutex_lock(&ctx->mutex);
	char *api_key = daydream_auth_get_api_key(ctx->auth);
	uint32_t target_fps = ctx->target_fps;
	pthread_mutex_unlock(&ctx->mutex);

	if (!api_key) {
		pthread_mutex_lock(&ctx->mutex);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	// Create stream orchestrator with adapter callbacks
	// Note: WHEP is handled separately to maintain async connection
	daydream_stream_ops_t ops = create_stream_ops(ctx);
	ops.whep_create = NULL; // Don't let stream module handle WHEP
	ops.whep_connect = NULL;
	ops.whep_disconnect = NULL;
	ops.whep_destroy = NULL;

	struct daydream_stream_config config = {
		.width = STREAM_SIZE,
		.height = STREAM_SIZE,
		.fps = target_fps,
		.bitrate = 500000,
	};

	// Destroy old stream if exists
	if (ctx->stream) {
		daydream_stream_destroy(ctx->stream);
	}
	ctx->stream = daydream_stream_create(&config, &ops);

	if (!ctx->stream) {
		blog(LOG_ERROR, "[Daydream] Failed to create stream orchestrator");
		bfree(api_key);
		pthread_mutex_lock(&ctx->mutex);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	// Start the stream (API call, encoder, decoder, WHIP)
	bool success = daydream_stream_start(ctx->stream, api_key);
	bfree(api_key);

	if (!success || ctx->stopping) {
		const char *error = daydream_stream_get_error(ctx->stream);
		blog(LOG_ERROR, "[Daydream] Stream start failed: %s", error ? error : "unknown");
		pthread_mutex_lock(&ctx->mutex);
		ctx->start_thread_running = false;
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	// Stream started successfully, now set up filter-specific state
	pthread_mutex_lock(&ctx->mutex);
	ctx->streaming = true;
	ctx->stopping = false;
	ctx->frame_count = 0;
	ctx->last_encode_time = os_gettime_ns();
	ctx->first_frame_rendered = false;

	// Reset frame skip state
	daydream_frameskip_reset(ctx->frameskip);

	// Start encode thread
	ctx->encode_thread_running = true;
	pthread_create(&ctx->encode_thread, NULL, encode_thread_func, ctx);

	// Start update thread for live parameter updates
	ctx->update_thread_running = true;
	pthread_create(&ctx->update_thread, NULL, update_thread_func, ctx);

	// Handle WHEP separately (async connection)
	const char *whep_url = daydream_whip_get_whep_url(ctx->whip);
	if (whep_url) {
		bfree(ctx->whep_url);
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

	ctx->start_thread_running = false;
	pthread_mutex_unlock(&ctx->mutex);

	// Trigger UI refresh to update streaming button
	obs_source_update_properties(ctx->source);

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

	char *api_key = daydream_auth_get_api_key(ctx->auth);
	if (!api_key || strlen(api_key) == 0) {
		bfree(api_key);
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}
	bfree(api_key);

	ctx->start_thread_running = true;
	ctx->stopping = false;
	pthread_create(&ctx->start_thread, NULL, start_streaming_thread_func, ctx);

	pthread_mutex_unlock(&ctx->mutex);

	// Force UI refresh to show disabled button
	obs_source_update_properties(ctx->source);
	return true;
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

static bool on_streaming_toggle_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct daydream_filter *ctx = data;

	if (ctx->streaming) {
		on_stop_clicked(props, property, data);
	} else {
		on_start_clicked(props, property, data);
	}

	// Return true to refresh properties UI (updates button label)
	return true;
}

static obs_properties_t *daydream_filter_get_properties(void *data)
{
	struct daydream_filter *ctx = data;
	obs_properties_t *props = obs_properties_create();
	bool logged_in = daydream_auth_is_logged_in(ctx->auth);
	bool is_streaming = ctx->streaming;

	obs_properties_add_text(props, "title_header", "✦ Daydream ✦", OBS_TEXT_INFO);

	const char *auth_label = logged_in ? "Logout" : "Login with Daydream";
	obs_properties_add_button(props, PROP_LOGIN, auth_label, on_auth_toggle_clicked);

	// --- Streaming Control ---
	bool is_transitioning = ctx->start_thread_running || ctx->stopping;
	blog(LOG_INFO,
	     "[Daydream] get_properties: streaming=%d, start_thread_running=%d, stopping=%d, transitioning=%d",
	     ctx->streaming, ctx->start_thread_running, ctx->stopping, is_transitioning);
	const char *toggle_label = is_streaming ? "Stop Streaming" : "Start Streaming";
	obs_property_t *toggle =
		obs_properties_add_button(props, PROP_START, toggle_label, on_streaming_toggle_clicked);
	obs_property_set_enabled(toggle, logged_in && !is_transitioning);

	obs_properties_add_text(props, "model_header", "\n\n【 Model 】", OBS_TEXT_INFO);

	// Cold parameter: disabled during streaming
	obs_property_t *model =
		obs_properties_add_list(props, PROP_MODEL, "Model", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(model, "SDXL Turbo", "stabilityai/sdxl-turbo");
	obs_property_list_add_string(model, "SD Turbo", "stabilityai/sd-turbo");
	obs_property_list_add_string(model, "Dreamshaper 8", "Lykon/dreamshaper-8");
	obs_property_list_add_string(model, "Openjourney v4", "prompthero/openjourney-v4");
	obs_property_set_enabled(model, logged_in && !is_streaming);
	obs_property_set_modified_callback(model, on_model_changed);

	// --- Prompt Schedule ---
	obs_properties_add_text(props, "prompt_header", "\n\n【 Prompt Schedule 】", OBS_TEXT_INFO);

	obs_property_t *prompt_count = obs_properties_add_int_slider(props, PROP_PROMPT_COUNT, "Prompt Count", 1, 4, 1);
	obs_property_set_enabled(prompt_count, logged_in);
	obs_property_set_modified_callback(prompt_count, on_prompt_count_changed);

	obs_property_t *p1 = obs_properties_add_text(props, PROP_PROMPT_1, "Prompt 1", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(p1, logged_in);
	obs_property_t *p1w = obs_properties_add_float_slider(props, PROP_PROMPT_1_WEIGHT, "Weight 1", 0.0, 1.0, 0.01);
	obs_property_set_enabled(p1w, logged_in);

	obs_property_t *p2 = obs_properties_add_text(props, PROP_PROMPT_2, "Prompt 2", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(p2, logged_in);
	obs_property_set_visible(p2, false);
	obs_property_t *p2w = obs_properties_add_float_slider(props, PROP_PROMPT_2_WEIGHT, "Weight 2", 0.0, 1.0, 0.01);
	obs_property_set_enabled(p2w, logged_in);
	obs_property_set_visible(p2w, false);

	obs_property_t *p3 = obs_properties_add_text(props, PROP_PROMPT_3, "Prompt 3", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(p3, logged_in);
	obs_property_set_visible(p3, false);
	obs_property_t *p3w = obs_properties_add_float_slider(props, PROP_PROMPT_3_WEIGHT, "Weight 3", 0.0, 1.0, 0.01);
	obs_property_set_enabled(p3w, logged_in);
	obs_property_set_visible(p3w, false);

	obs_property_t *p4 = obs_properties_add_text(props, PROP_PROMPT_4, "Prompt 4", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(p4, logged_in);
	obs_property_set_visible(p4, false);
	obs_property_t *p4w = obs_properties_add_float_slider(props, PROP_PROMPT_4_WEIGHT, "Weight 4", 0.0, 1.0, 0.01);
	obs_property_set_enabled(p4w, logged_in);
	obs_property_set_visible(p4w, false);

	obs_property_t *prompt_interp = obs_properties_add_list(props, PROP_PROMPT_INTERP, "Prompt Interpolation",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prompt_interp, "Slerp", "slerp");
	obs_property_list_add_string(prompt_interp, "Linear", "linear");
	obs_property_set_enabled(prompt_interp, logged_in);
	obs_property_set_visible(prompt_interp, false);

	obs_property_t *norm_prompt = obs_properties_add_bool(props, PROP_NORMALIZE_PROMPT, "Normalize Prompt Weights");
	obs_property_set_enabled(norm_prompt, logged_in);
	obs_property_set_visible(norm_prompt, false);

	obs_property_t *neg_prompt =
		obs_properties_add_text(props, PROP_NEGATIVE_PROMPT, "Negative Prompt", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(neg_prompt, logged_in);

	// --- Seed Schedule ---
	obs_properties_add_text(props, "seed_header", "\n\n【 Seed Schedule 】", OBS_TEXT_INFO);

	obs_property_t *seed_count = obs_properties_add_int_slider(props, PROP_SEED_COUNT, "Seed Count", 1, 4, 1);
	obs_property_set_enabled(seed_count, logged_in);
	obs_property_set_modified_callback(seed_count, on_seed_count_changed);

	obs_property_t *s1 = obs_properties_add_int(props, PROP_SEED_1, "Seed 1", 0, INT_MAX, 1);
	obs_property_set_enabled(s1, logged_in);
	obs_property_t *s1w = obs_properties_add_float_slider(props, PROP_SEED_1_WEIGHT, "Weight 1", 0.0, 1.0, 0.01);
	obs_property_set_enabled(s1w, logged_in);

	obs_property_t *s2 = obs_properties_add_int(props, PROP_SEED_2, "Seed 2", 0, INT_MAX, 1);
	obs_property_set_enabled(s2, logged_in);
	obs_property_set_visible(s2, false);
	obs_property_t *s2w = obs_properties_add_float_slider(props, PROP_SEED_2_WEIGHT, "Weight 2", 0.0, 1.0, 0.01);
	obs_property_set_enabled(s2w, logged_in);
	obs_property_set_visible(s2w, false);

	obs_property_t *s3 = obs_properties_add_int(props, PROP_SEED_3, "Seed 3", 0, INT_MAX, 1);
	obs_property_set_enabled(s3, logged_in);
	obs_property_set_visible(s3, false);
	obs_property_t *s3w = obs_properties_add_float_slider(props, PROP_SEED_3_WEIGHT, "Weight 3", 0.0, 1.0, 0.01);
	obs_property_set_enabled(s3w, logged_in);
	obs_property_set_visible(s3w, false);

	obs_property_t *s4 = obs_properties_add_int(props, PROP_SEED_4, "Seed 4", 0, INT_MAX, 1);
	obs_property_set_enabled(s4, logged_in);
	obs_property_set_visible(s4, false);
	obs_property_t *s4w = obs_properties_add_float_slider(props, PROP_SEED_4_WEIGHT, "Weight 4", 0.0, 1.0, 0.01);
	obs_property_set_enabled(s4w, logged_in);
	obs_property_set_visible(s4w, false);

	obs_property_t *seed_interp = obs_properties_add_list(props, PROP_SEED_INTERP, "Seed Interpolation",
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(seed_interp, "Slerp", "slerp");
	obs_property_list_add_string(seed_interp, "Linear", "linear");
	obs_property_set_enabled(seed_interp, logged_in);
	obs_property_set_visible(seed_interp, false);

	obs_property_t *norm_seed = obs_properties_add_bool(props, PROP_NORMALIZE_SEED, "Normalize Seed Weights");
	obs_property_set_enabled(norm_seed, logged_in);
	obs_property_set_visible(norm_seed, false);

	// --- Step Schedule (t_index_list) ---
	obs_properties_add_text(props, "step_header", "\n\n【 Step Schedule 】", OBS_TEXT_INFO);

	// Cold parameter: disabled during streaming
	obs_property_t *num_steps =
		obs_properties_add_int_slider(props, PROP_NUM_STEPS, "Num Inference Steps", 1, 100, 1);
	obs_property_set_enabled(num_steps, logged_in && !is_streaming);

	obs_property_t *step_count = obs_properties_add_int_slider(props, PROP_STEP_COUNT, "Step Count", 1, 4, 1);
	obs_property_set_enabled(step_count, logged_in);
	obs_property_set_modified_callback(step_count, on_step_count_changed);

	obs_property_t *st1 = obs_properties_add_int_slider(props, PROP_STEP_1, "Step 1 (t_index)", 0, 50, 1);
	obs_property_set_enabled(st1, logged_in);

	obs_property_t *st2 = obs_properties_add_int_slider(props, PROP_STEP_2, "Step 2 (t_index)", 0, 50, 1);
	obs_property_set_enabled(st2, logged_in);
	obs_property_set_visible(st2, false);

	obs_property_t *st3 = obs_properties_add_int_slider(props, PROP_STEP_3, "Step 3 (t_index)", 0, 50, 1);
	obs_property_set_enabled(st3, logged_in);
	obs_property_set_visible(st3, false);

	obs_property_t *st4 = obs_properties_add_int_slider(props, PROP_STEP_4, "Step 4 (t_index)", 0, 50, 1);
	obs_property_set_enabled(st4, logged_in);
	obs_property_set_visible(st4, false);

	// --- Generation ---
	obs_properties_add_text(props, "gen_header", "\n\n【 Generation 】", OBS_TEXT_INFO);

	obs_property_t *guidance = obs_properties_add_float_slider(props, PROP_GUIDANCE, "Guidance", 0.1, 20.0, 0.1);
	obs_property_set_enabled(guidance, logged_in);

	obs_property_t *delta = obs_properties_add_float_slider(props, PROP_DELTA, "Delta", 0.0, 1.0, 0.01);
	obs_property_set_enabled(delta, logged_in);

	// Cold parameter: disabled during streaming
	obs_property_t *add_noise = obs_properties_add_bool(props, PROP_ADD_NOISE, "Add Noise");
	obs_property_set_enabled(add_noise, logged_in && !is_streaming);

	// --- IP Adapter ---
	obs_properties_add_text(props, "ip_adapter_header", "\n\n【 IP Adapter 】", OBS_TEXT_INFO);

	obs_property_t *ip_enabled = obs_properties_add_bool(props, PROP_IP_ADAPTER_ENABLED, "Enable IP Adapter");
	obs_property_set_enabled(ip_enabled, logged_in);

	obs_property_t *ip_scale =
		obs_properties_add_float_slider(props, PROP_IP_ADAPTER_SCALE, "IP Adapter Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(ip_scale, logged_in);

	// Cold parameter: disabled during streaming
	obs_property_t *ip_type = obs_properties_add_list(props, PROP_IP_ADAPTER_TYPE, "IP Adapter Type",
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(ip_type, "Regular", "regular");
	obs_property_list_add_string(ip_type, "FaceID", "faceid");
	obs_property_set_enabled(ip_type, logged_in && !is_streaming);

	obs_property_t *style_url =
		obs_properties_add_text(props, PROP_STYLE_IMAGE_URL, "Style Image URL", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(style_url, logged_in);

	// --- ControlNet ---
	obs_properties_add_text(props, "controlnet_header", "\n\n【 ControlNet 】", OBS_TEXT_INFO);

	obs_property_t *depth_scale =
		obs_properties_add_float_slider(props, PROP_DEPTH_SCALE, "Depth Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(depth_scale, logged_in);

	obs_property_t *canny_scale =
		obs_properties_add_float_slider(props, PROP_CANNY_SCALE, "Canny Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(canny_scale, logged_in);

	obs_property_t *tile_scale =
		obs_properties_add_float_slider(props, PROP_TILE_SCALE, "Tile Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(tile_scale, logged_in);

	obs_property_t *openpose_scale =
		obs_properties_add_float_slider(props, PROP_OPENPOSE_SCALE, "Openpose Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(openpose_scale, logged_in);
	obs_property_set_visible(openpose_scale, false);

	obs_property_t *hed_scale = obs_properties_add_float_slider(props, PROP_HED_SCALE, "HED Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(hed_scale, logged_in);
	obs_property_set_visible(hed_scale, false);

	obs_property_t *color_scale =
		obs_properties_add_float_slider(props, PROP_COLOR_SCALE, "Color Scale", 0.0, 1.0, 0.01);
	obs_property_set_enabled(color_scale, logged_in);
	obs_property_set_visible(color_scale, false);

	// --- Experimental ---
	obs_properties_add_text(props, "experimental_header", "\n\n【 Experimental 】", OBS_TEXT_INFO);

	obs_property_t *frame_skip =
		obs_properties_add_bool(props, PROP_FRAME_SKIP_ENABLED, "Skip Out-of-Order Frames");
	obs_property_set_enabled(frame_skip, logged_in);

	obs_property_t *blur_size =
		obs_properties_add_int_slider(props, PROP_BLUR_SIZE, "Background Blur (0=off)", 0, 64, 4);
	obs_property_set_enabled(blur_size, logged_in);

	// --- About ---
	obs_properties_add_text(props, "about_header", "\n\n【 About 】", OBS_TEXT_INFO);

	char version_buf[64];
	snprintf(version_buf, sizeof(version_buf), "Version %s", PLUGIN_VERSION);
	obs_properties_add_text(props, "about_version", version_buf, OBS_TEXT_INFO);
	obs_properties_add_button(props, "about_homepage", "Homepage", on_homepage_clicked);
	obs_properties_add_button(props, "about_github", "GitHub", on_github_clicked);

	return props;
}

static void daydream_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, PROP_MODEL, "stabilityai/sdxl-turbo");
	obs_data_set_default_string(settings, PROP_NEGATIVE_PROMPT, "blurry, low quality, flat, 2d");
	obs_data_set_default_double(settings, PROP_GUIDANCE, 1.0);
	obs_data_set_default_double(settings, PROP_DELTA, 0.7);
	obs_data_set_default_bool(settings, PROP_ADD_NOISE, true);

	// Prompt Schedule defaults
	obs_data_set_default_int(settings, PROP_PROMPT_COUNT, 1);
	obs_data_set_default_string(settings, PROP_PROMPT_1,
				    "cute shiba inu, studio ghibli style, anime, soft lighting");
	obs_data_set_default_double(settings, PROP_PROMPT_1_WEIGHT, 1.0);
	obs_data_set_default_string(settings, PROP_PROMPT_2, "");
	obs_data_set_default_double(settings, PROP_PROMPT_2_WEIGHT, 1.0);
	obs_data_set_default_string(settings, PROP_PROMPT_3, "");
	obs_data_set_default_double(settings, PROP_PROMPT_3_WEIGHT, 1.0);
	obs_data_set_default_string(settings, PROP_PROMPT_4, "");
	obs_data_set_default_double(settings, PROP_PROMPT_4_WEIGHT, 1.0);
	obs_data_set_default_string(settings, PROP_PROMPT_INTERP, "slerp");
	obs_data_set_default_bool(settings, PROP_NORMALIZE_PROMPT, true);

	// Seed Schedule defaults
	obs_data_set_default_int(settings, PROP_SEED_COUNT, 1);
	obs_data_set_default_int(settings, PROP_SEED_1, 42);
	obs_data_set_default_double(settings, PROP_SEED_1_WEIGHT, 1.0);
	obs_data_set_default_int(settings, PROP_SEED_2, 0);
	obs_data_set_default_double(settings, PROP_SEED_2_WEIGHT, 1.0);
	obs_data_set_default_int(settings, PROP_SEED_3, 0);
	obs_data_set_default_double(settings, PROP_SEED_3_WEIGHT, 1.0);
	obs_data_set_default_int(settings, PROP_SEED_4, 0);
	obs_data_set_default_double(settings, PROP_SEED_4_WEIGHT, 1.0);
	obs_data_set_default_string(settings, PROP_SEED_INTERP, "slerp");
	obs_data_set_default_bool(settings, PROP_NORMALIZE_SEED, true);

	// Step Schedule defaults (t_index_list)
	obs_data_set_default_int(settings, PROP_NUM_STEPS, 50);
	obs_data_set_default_int(settings, PROP_STEP_COUNT, 1);
	obs_data_set_default_int(settings, PROP_STEP_1, 11);
	obs_data_set_default_int(settings, PROP_STEP_2, 0);
	obs_data_set_default_int(settings, PROP_STEP_3, 0);
	obs_data_set_default_int(settings, PROP_STEP_4, 0);

	// IP Adapter defaults
	obs_data_set_default_bool(settings, PROP_IP_ADAPTER_ENABLED, true);
	obs_data_set_default_double(settings, PROP_IP_ADAPTER_SCALE, 0.5);
	obs_data_set_default_string(settings, PROP_IP_ADAPTER_TYPE, "regular");
	obs_data_set_default_string(settings, PROP_STYLE_IMAGE_URL, "");

	// ControlNet defaults
	obs_data_set_default_double(settings, PROP_DEPTH_SCALE, 0.45);
	obs_data_set_default_double(settings, PROP_CANNY_SCALE, 0.0);
	obs_data_set_default_double(settings, PROP_TILE_SCALE, 0.21);
	obs_data_set_default_double(settings, PROP_OPENPOSE_SCALE, 0.0);
	obs_data_set_default_double(settings, PROP_HED_SCALE, 0.0);
	obs_data_set_default_double(settings, PROP_COLOR_SCALE, 0.0);

	// Experimental defaults
	obs_data_set_default_bool(settings, PROP_FRAME_SKIP_ENABLED, true);
	obs_data_set_default_int(settings, PROP_BLUR_SIZE, 2);
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
