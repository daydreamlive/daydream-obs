#pragma once

#include <stdbool.h>

typedef void (*daydream_auth_callback)(bool success, const char *api_key, const char *error, void *userdata);

struct daydream_auth;

struct daydream_auth *daydream_auth_create(void);
void daydream_auth_destroy(struct daydream_auth *auth);

bool daydream_auth_is_logged_in(struct daydream_auth *auth);
const char *daydream_auth_get_api_key(struct daydream_auth *auth);

void daydream_auth_login(struct daydream_auth *auth, daydream_auth_callback callback, void *userdata);
void daydream_auth_logout(struct daydream_auth *auth);

bool daydream_auth_load_credentials(struct daydream_auth *auth);
bool daydream_auth_save_credentials(struct daydream_auth *auth, const char *api_key);
