#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAYDREAM_MAX_SCHEDULE_SLOTS 4

// Update flags for live parameter updates (bitmask)
#define UPDATE_FLAG_PROMPT (1ULL << 0)
#define UPDATE_FLAG_NEGATIVE_PROMPT (1ULL << 1)
#define UPDATE_FLAG_SEED (1ULL << 2)
#define UPDATE_FLAG_STEP_SCHEDULE (1ULL << 3)
#define UPDATE_FLAG_GUIDANCE (1ULL << 4)
#define UPDATE_FLAG_DELTA (1ULL << 5)
#define UPDATE_FLAG_CONTROLNETS (1ULL << 6)
#define UPDATE_FLAG_IP_ADAPTER (1ULL << 7)
#define UPDATE_FLAG_INTERP (1ULL << 8)

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

	// Schedules
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

// Update stream parameters (PATCH request)
// Only fields indicated by update_flags will be sent
// Returns true on success, false on failure
bool daydream_api_update_stream(const char *api_key, const char *stream_id, const struct daydream_stream_params *params,
				uint64_t update_flags);

void daydream_api_free_result(struct daydream_stream_result *result);
