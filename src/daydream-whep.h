#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daydream_whep;

typedef void (*daydream_whep_frame_callback)(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe,
					     void *userdata);

typedef void (*daydream_whep_state_callback)(bool connected, const char *error, void *userdata);

struct daydream_whep_config {
	const char *whep_url;
	const char *api_key;
	daydream_whep_frame_callback on_frame;
	daydream_whep_state_callback on_state;
	void *userdata;
};

struct daydream_whep *daydream_whep_create(const struct daydream_whep_config *config);
void daydream_whep_destroy(struct daydream_whep *whep);

bool daydream_whep_connect(struct daydream_whep *whep);
void daydream_whep_disconnect(struct daydream_whep *whep);
bool daydream_whep_is_connected(struct daydream_whep *whep);

#ifdef __cplusplus
}
#endif
