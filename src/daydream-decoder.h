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

#if defined(__APPLE__)
	// Zero-copy decode: CVPixelBuffer containing GPU-resident NV12 data
	// When non-NULL, y_data/uv_data are NULL and caller should use
	// CVMetalTextureCache to create textures directly from this buffer.
	// Caller must call daydream_decoder_release_frame() when done.
	void *cv_pixel_buffer; // CVPixelBufferRef (retained, caller must release)
	void *iosurface;       // IOSurfaceRef (not retained, valid while cv_pixel_buffer is)
#endif
};

struct daydream_decoder *daydream_decoder_create(const struct daydream_decoder_config *config);
void daydream_decoder_destroy(struct daydream_decoder *decoder);

bool daydream_decoder_decode(struct daydream_decoder *decoder, const uint8_t *h264_data, size_t size,
			     struct daydream_decoded_frame *out_frame);

#if defined(__APPLE__)
/**
 * Release a zero-copy decoded frame.
 * Must be called after rendering when cv_pixel_buffer is non-NULL.
 * Safe to call with NULL or frames without cv_pixel_buffer.
 */
void daydream_decoder_release_frame(struct daydream_decoded_frame *frame);

/**
 * Check if decoder is using zero-copy mode.
 */
bool daydream_decoder_is_zerocopy(struct daydream_decoder *decoder);
#endif

#ifdef __cplusplus
}
#endif
