#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daydream_encoder;

struct daydream_encoder_config {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t bitrate;
};

struct daydream_encoded_frame {
	uint8_t *data;
	size_t size;
	bool is_keyframe;
	int64_t pts;
};

struct daydream_encoder *daydream_encoder_create(const struct daydream_encoder_config *config);
void daydream_encoder_destroy(struct daydream_encoder *encoder);

bool daydream_encoder_encode(struct daydream_encoder *encoder, const uint8_t *bgra_data, uint32_t linesize,
			     struct daydream_encoded_frame *out_frame);

void daydream_encoder_request_keyframe(struct daydream_encoder *encoder);

#ifdef __cplusplus
}
#endif
