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
	// NV12 format: Y plane + interleaved UV plane
	uint8_t *y_data;      // Y plane (full resolution)
	uint8_t *uv_data;     // UV plane (half resolution, interleaved)
	uint32_t y_linesize;  // Y plane linesize
	uint32_t uv_linesize; // UV plane linesize
	uint32_t width;
	uint32_t height;
	int64_t pts;

	// Legacy BGRA support (for SW fallback)
	uint8_t *bgra_data;
	uint32_t bgra_linesize;
	bool is_nv12; // true = NV12, false = BGRA
};

struct daydream_decoder *daydream_decoder_create(const struct daydream_decoder_config *config);
void daydream_decoder_destroy(struct daydream_decoder *decoder);

bool daydream_decoder_decode(struct daydream_decoder *decoder, const uint8_t *h264_data, size_t size,
			     struct daydream_decoded_frame *out_frame);

#ifdef __cplusplus
}
#endif
