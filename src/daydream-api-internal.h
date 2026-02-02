#pragma once

// Internal API functions exposed for testing
// Only include this header in test code with DAYDREAM_TESTING defined

#ifdef DAYDREAM_TESTING

#include "daydream-api.h"
#include <cJSON.h>

// JSON builder functions (normally static in daydream-api.c)
cJSON *build_controlnets_json(const char *model, const struct daydream_controlnet_params *controlnets);
cJSON *build_prompt_json(const struct daydream_prompt_schedule *schedule);
cJSON *build_seed_json(const struct daydream_seed_schedule *schedule);
cJSON *build_step_schedule_json(const struct daydream_step_schedule *schedule);
cJSON *build_ip_adapter_json(const struct daydream_ip_adapter_params *ip_adapter);

#endif // DAYDREAM_TESTING
