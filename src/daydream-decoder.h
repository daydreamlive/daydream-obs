#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daydream_decoder;

struct daydream_decoder_config {
	uint32_t width;
	uint32_t height;
};

struct daydream_decoded_frame {
	uint8_t *data;
	uint32_t linesize;
	uint32_t width;
	uint32_t height;
	int64_t pts;
};

struct daydream_decoder *daydream_decoder_create(const struct daydream_decoder_config *config);
void daydream_decoder_destroy(struct daydream_decoder *decoder);

bool daydream_decoder_decode(struct daydream_decoder *decoder, const uint8_t *h264_data, size_t size,
			     struct daydream_decoded_frame *out_frame);

#ifdef __cplusplus
}
#endif
