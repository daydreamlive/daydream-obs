#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DAYDREAM_MAX_SCHEDULE_SLOTS 4

struct daydream_ip_adapter_params {
	bool enabled;
	float scale;
	const char *type; // "regular" or "faceid"
	const char *style_image_url;
};

struct daydream_controlnet_params {
	float depth_scale;
	float canny_scale;
	float tile_scale;
	float openpose_scale;
	float hed_scale;
	float color_scale;
};

struct daydream_prompt_schedule {
	int count;
	const char *prompts[DAYDREAM_MAX_SCHEDULE_SLOTS];
	float weights[DAYDREAM_MAX_SCHEDULE_SLOTS];
};

struct daydream_seed_schedule {
	int count;
	int seeds[DAYDREAM_MAX_SCHEDULE_SLOTS];
	float weights[DAYDREAM_MAX_SCHEDULE_SLOTS];
};

struct daydream_step_schedule {
	int count;
	int steps[DAYDREAM_MAX_SCHEDULE_SLOTS]; // t_index_list values
};

struct daydream_stream_params {
	const char *model_id;
	const char *negative_prompt;
	float guidance;
	float delta;
	int num_inference_steps; // for building the schedule grid
	int width;
	int height;

	// Schedules (TouchDesigner-equivalent)
	struct daydream_prompt_schedule prompt_schedule;
	struct daydream_seed_schedule seed_schedule;
	struct daydream_step_schedule step_schedule;

	// Other parameters
	bool do_add_noise;
	struct daydream_ip_adapter_params ip_adapter;
	const char *prompt_interpolation_method; // "slerp" or "linear"
	bool normalize_prompt_weights;
	const char *seed_interpolation_method; // "slerp" or "linear"
	bool normalize_seed_weights;
	struct daydream_controlnet_params controlnets;
};

struct daydream_stream_result {
	char *stream_id;
	char *whip_url;
	char *whep_url;
	char *error;
	bool success;
};

void daydream_api_init(void);
void daydream_api_cleanup(void);

struct daydream_stream_result daydream_api_create_stream(const char *api_key,
							 const struct daydream_stream_params *params);

void daydream_api_free_result(struct daydream_stream_result *result);
