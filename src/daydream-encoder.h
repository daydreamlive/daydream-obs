#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct daydream_encoder;

struct daydream_encoder_config {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t bitrate;
	bool use_zerocopy; // macOS only: use IOSurface zero-copy path
};

struct daydream_encoded_frame {
	uint8_t *data;
	size_t size;
	bool is_keyframe;
	int64_t pts;
};

struct daydream_encoder *daydream_encoder_create(const struct daydream_encoder_config *config);
void daydream_encoder_destroy(struct daydream_encoder *encoder);

// Standard encode path (CPU BGRA buffer)
bool daydream_encoder_encode(struct daydream_encoder *encoder, const uint8_t *bgra_data, uint32_t linesize,
			     struct daydream_encoded_frame *out_frame);

// Adaptive bitrate control
bool daydream_encoder_set_bitrate(struct daydream_encoder *encoder, uint32_t bitrate);
uint32_t daydream_encoder_get_bitrate(struct daydream_encoder *encoder);
void daydream_encoder_request_keyframe(struct daydream_encoder *encoder);

#if defined(__APPLE__)
// Zero-copy encode path (macOS only)
// Returns IOSurface that can be used as render target, NULL if not using zero-copy
IOSurfaceRef daydream_encoder_get_iosurface(struct daydream_encoder *encoder);

// Encode from IOSurface (zero-copy path)
bool daydream_encoder_encode_iosurface(struct daydream_encoder *encoder, struct daydream_encoded_frame *out_frame);

// Check if encoder is using zero-copy path
bool daydream_encoder_is_zerocopy(struct daydream_encoder *encoder);
#endif

#ifdef __cplusplus
}
#endif
