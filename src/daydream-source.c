#include "daydream-source.h"
#include <obs-module.h>
#include <graphics/graphics.h>

#define DAYDREAM_WIDTH 512
#define DAYDREAM_HEIGHT 512

struct daydream_source {
	obs_source_t *source;
	gs_texture_t *texture;
	uint32_t width;
	uint32_t height;
};

static const char *daydream_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Daydream";
}

static void *daydream_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct daydream_source *ctx = bzalloc(sizeof(struct daydream_source));
	ctx->source = source;
	ctx->width = DAYDREAM_WIDTH;
	ctx->height = DAYDREAM_HEIGHT;
	ctx->texture = NULL;

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

		ctx->texture = gs_texture_create(ctx->width, ctx->height,
						 GS_BGRA, 1,
						 (const uint8_t **)&pixels, 0);
		bfree(pixels);
	}

	if (ctx->texture) {
		gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(default_effect, "Draw");

		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"),
				      ctx->texture);
		gs_draw_sprite(ctx->texture, 0, ctx->width, ctx->height);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}
}

static struct obs_source_info daydream_source_info = {
	.id = "daydream_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = daydream_source_get_name,
	.create = daydream_source_create,
	.destroy = daydream_source_destroy,
	.get_width = daydream_source_get_width,
	.get_height = daydream_source_get_height,
	.video_render = daydream_source_video_render,
};

void daydream_source_register(void)
{
	obs_register_source(&daydream_source_info);
}
