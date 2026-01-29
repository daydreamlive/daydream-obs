#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daydream_whip;

typedef void (*daydream_whip_state_callback)(bool connected, const char *error, void *userdata);

struct daydream_whip_config {
	const char *whip_url;
	const char *api_key;
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	daydream_whip_state_callback on_state;
	void *userdata;
};

struct daydream_whip *daydream_whip_create(const struct daydream_whip_config *config);
void daydream_whip_destroy(struct daydream_whip *whip);

bool daydream_whip_connect(struct daydream_whip *whip);
void daydream_whip_disconnect(struct daydream_whip *whip);
bool daydream_whip_is_connected(struct daydream_whip *whip);

bool daydream_whip_send_frame(struct daydream_whip *whip, const uint8_t *h264_data, size_t size, uint32_t timestamp_ms,
			      bool is_keyframe);

const char *daydream_whip_get_whep_url(struct daydream_whip *whip);

#ifdef __cplusplus
}
#endif
