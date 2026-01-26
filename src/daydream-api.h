#pragma once

#include <stdbool.h>
#include <stddef.h>

struct daydream_stream_params {
	const char *model_id;
	const char *prompt;
	const char *negative_prompt;
	float guidance;
	float delta;
	int steps;
	int width;
	int height;
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
