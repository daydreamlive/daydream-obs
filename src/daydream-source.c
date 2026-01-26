#include "daydream-source.h"
#include "daydream-api.h"
#include "daydream-auth.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>

#define DAYDREAM_WIDTH 512
#define DAYDREAM_HEIGHT 512

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

struct daydream_source {
	obs_source_t *source;
	gs_texture_t *texture;
	uint32_t width;
	uint32_t height;

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

	pthread_mutex_t mutex;
};

static const char *daydream_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Daydream";
}

static void daydream_source_update(void *data, obs_data_t *settings)
{
	struct daydream_source *ctx = data;

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

static void *daydream_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct daydream_source *ctx = bzalloc(sizeof(struct daydream_source));
	ctx->source = source;
	ctx->width = DAYDREAM_WIDTH;
	ctx->height = DAYDREAM_HEIGHT;
	ctx->texture = NULL;
	ctx->streaming = false;

	pthread_mutex_init(&ctx->mutex, NULL);

	ctx->auth = daydream_auth_create();

	daydream_source_update(ctx, settings);

	return ctx;
}

static void daydream_source_destroy(void *data)
{
	struct daydream_source *ctx = data;

	if (ctx->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->texture);
		obs_leave_graphics();
	}

	daydream_auth_destroy(ctx->auth);

	bfree(ctx->prompt);
	bfree(ctx->negative_prompt);
	bfree(ctx->model);
	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);

	pthread_mutex_destroy(&ctx->mutex);

	bfree(ctx);
}

static uint32_t daydream_source_get_width(void *data)
{
	struct daydream_source *ctx = data;
	return ctx->width;
}

static uint32_t daydream_source_get_height(void *data)
{
	struct daydream_source *ctx = data;
	return ctx->height;
}

static void daydream_source_video_render(void *data, gs_effect_t *effect)
{
	struct daydream_source *ctx = data;
	UNUSED_PARAMETER(effect);

	if (!ctx->texture) {
		uint8_t *pixels = bzalloc(ctx->width * ctx->height * 4);
		for (uint32_t i = 0; i < ctx->width * ctx->height; i++) {
			pixels[i * 4 + 0] = 0;
			pixels[i * 4 + 1] = 0;
			pixels[i * 4 + 2] = 0;
			pixels[i * 4 + 3] = 255;
		}

		ctx->texture = gs_texture_create(ctx->width, ctx->height, GS_BGRA, 1, (const uint8_t **)&pixels, 0);
		bfree(pixels);
	}

	if (ctx->texture) {
		gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(default_effect, "Draw");

		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), ctx->texture);
		gs_draw_sprite(ctx->texture, 0, ctx->width, ctx->height);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}
}

static void on_login_callback(bool success, const char *api_key, const char *error, void *userdata)
{
	struct daydream_source *ctx = userdata;
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

	struct daydream_source *ctx = data;

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

	struct daydream_source *ctx = data;

	daydream_auth_logout(ctx->auth);
	obs_source_update_properties(ctx->source);

	return true;
}

static bool on_start_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->streaming) {
		blog(LOG_WARNING, "[Daydream] Already streaming");
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	const char *api_key = daydream_auth_get_api_key(ctx->auth);
	if (!api_key || strlen(api_key) == 0) {
		blog(LOG_ERROR, "[Daydream] Not logged in. Please login first.");
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	struct daydream_stream_params params = {
		.model_id = ctx->model,
		.prompt = ctx->prompt,
		.negative_prompt = ctx->negative_prompt,
		.guidance = ctx->guidance,
		.delta = ctx->delta,
		.steps = ctx->steps,
		.width = (int)ctx->width,
		.height = (int)ctx->height,
	};

	pthread_mutex_unlock(&ctx->mutex);

	struct daydream_stream_result result = daydream_api_create_stream(api_key, &params);

	pthread_mutex_lock(&ctx->mutex);

	if (result.success) {
		bfree(ctx->stream_id);
		bfree(ctx->whip_url);

		ctx->stream_id = bstrdup(result.stream_id);
		ctx->whip_url = bstrdup(result.whip_url);
		ctx->streaming = true;

		blog(LOG_INFO, "[Daydream] Streaming started!");
	} else {
		blog(LOG_ERROR, "[Daydream] Failed to start stream: %s", result.error ? result.error : "Unknown error");
	}

	pthread_mutex_unlock(&ctx->mutex);

	daydream_api_free_result(&result);

	return false;
}

static bool on_stop_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct daydream_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (!ctx->streaming) {
		blog(LOG_WARNING, "[Daydream] Not streaming");
		pthread_mutex_unlock(&ctx->mutex);
		return false;
	}

	ctx->streaming = false;

	bfree(ctx->stream_id);
	bfree(ctx->whip_url);
	bfree(ctx->whep_url);
	ctx->stream_id = NULL;
	ctx->whip_url = NULL;
	ctx->whep_url = NULL;

	blog(LOG_INFO, "[Daydream] Streaming stopped");

	pthread_mutex_unlock(&ctx->mutex);

	return false;
}

static obs_properties_t *daydream_source_get_properties(void *data)
{
	struct daydream_source *ctx = data;

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
	obs_property_set_enabled(start, logged_in);

	obs_property_t *stop = obs_properties_add_button(props, PROP_STOP, "Stop Streaming", on_stop_clicked);
	obs_property_set_enabled(stop, logged_in);

	return props;
}

static void daydream_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, PROP_MODEL, "stabilityai/sdxl-turbo");
	obs_data_set_default_string(settings, PROP_PROMPT, "a beautiful landscape");
	obs_data_set_default_string(settings, PROP_NEGATIVE_PROMPT, "blurry, low quality");
	obs_data_set_default_double(settings, PROP_GUIDANCE, 1.0);
	obs_data_set_default_double(settings, PROP_DELTA, 0.7);
	obs_data_set_default_int(settings, PROP_STEPS, 50);
}

static struct obs_source_info daydream_source_info = {
	.id = "daydream_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = daydream_source_get_name,
	.create = daydream_source_create,
	.destroy = daydream_source_destroy,
	.update = daydream_source_update,
	.get_width = daydream_source_get_width,
	.get_height = daydream_source_get_height,
	.video_render = daydream_source_video_render,
	.get_properties = daydream_source_get_properties,
	.get_defaults = daydream_source_get_defaults,
};

void daydream_source_register(void)
{
	obs_register_source(&daydream_source_info);
}
