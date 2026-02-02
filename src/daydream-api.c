#include "daydream-api.h"
#include <obs-module.h>
#include <curl/curl.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

#define DAYDREAM_API_BASE "https://api.daydream.live/v1"

struct response_buffer {
	char *data;
	size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct response_buffer *buf = (struct response_buffer *)userp;

	char *ptr = realloc(buf->data, buf->size + realsize + 1);
	if (!ptr)
		return 0;

	buf->data = ptr;
	memcpy(&(buf->data[buf->size]), contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = 0;

	return realsize;
}

// Helper to get string from JSON object (returns NULL if not found)
static char *json_get_string(cJSON *json, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
	if (cJSON_IsString(item) && item->valuestring) {
		return strdup(item->valuestring);
	}
	return NULL;
}

// Build ControlNets array based on model
static cJSON *build_controlnets_json(const char *model, const struct daydream_controlnet_params *controlnets)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	if (strcmp(model, "stabilityai/sd-turbo") == 0) {
		// SD Turbo (SD2.1): depth, canny, hed, openpose, color
		const struct {
			const char *model_id;
			const char *preprocessor;
			float scale;
		} configs[] = {
			{"thibaud/controlnet-sd21-depth-diffusers", "depth_tensorrt", controlnets->depth_scale},
			{"thibaud/controlnet-sd21-canny-diffusers", "canny", controlnets->canny_scale},
			{"thibaud/controlnet-sd21-hed-diffusers", "hed", controlnets->hed_scale},
			{"thibaud/controlnet-sd21-openpose-diffusers", "openpose", controlnets->openpose_scale},
			{"thibaud/controlnet-sd21-color-diffusers", "passthrough", controlnets->color_scale},
		};
		for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "model_id", configs[i].model_id);
			cJSON_AddNumberToObject(item, "conditioning_scale", configs[i].scale);
			cJSON_AddStringToObject(item, "preprocessor", configs[i].preprocessor);
			cJSON_AddObjectToObject(item, "preprocessor_params");
			cJSON_AddBoolToObject(item, "enabled", true);
			cJSON_AddItemToArray(arr, item);
		}
	} else if (strcmp(model, "stabilityai/sdxl-turbo") == 0) {
		// SDXL Turbo: depth, canny, tile
		const struct {
			const char *model_id;
			const char *preprocessor;
			float scale;
		} configs[] = {
			{"xinsir/controlnet-depth-sdxl-1.0", "depth_tensorrt", controlnets->depth_scale},
			{"xinsir/controlnet-canny-sdxl-1.0", "canny", controlnets->canny_scale},
			{"xinsir/controlnet-tile-sdxl-1.0", "feedback", controlnets->tile_scale},
		};
		for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "model_id", configs[i].model_id);
			cJSON_AddNumberToObject(item, "conditioning_scale", configs[i].scale);
			cJSON_AddStringToObject(item, "preprocessor", configs[i].preprocessor);
			cJSON_AddObjectToObject(item, "preprocessor_params");
			cJSON_AddBoolToObject(item, "enabled", true);
			cJSON_AddItemToArray(arr, item);
		}
	} else {
		// SD1.5 models: depth, canny, tile
		const struct {
			const char *model_id;
			const char *preprocessor;
			float scale;
		} configs[] = {
			{"lllyasviel/control_v11f1p_sd15_depth", "depth_tensorrt", controlnets->depth_scale},
			{"lllyasviel/control_v11p_sd15_canny", "canny", controlnets->canny_scale},
			{"lllyasviel/control_v11f1e_sd15_tile", "feedback", controlnets->tile_scale},
		};
		for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "model_id", configs[i].model_id);
			cJSON_AddNumberToObject(item, "conditioning_scale", configs[i].scale);
			cJSON_AddStringToObject(item, "preprocessor", configs[i].preprocessor);
			cJSON_AddObjectToObject(item, "preprocessor_params");
			cJSON_AddBoolToObject(item, "enabled", true);
			cJSON_AddItemToArray(arr, item);
		}
	}

	return arr;
}

// Build prompt JSON (string or weighted array)
static cJSON *build_prompt_json(const struct daydream_prompt_schedule *schedule)
{
	if (schedule->count <= 1) {
		const char *p = (schedule->count == 1 && schedule->prompts[0]) ? schedule->prompts[0] : "strawberry";
		return cJSON_CreateString(p);
	}

	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < schedule->count; i++) {
		cJSON *pair = cJSON_CreateArray();
		cJSON_AddItemToArray(pair, cJSON_CreateString(schedule->prompts[i] ? schedule->prompts[i] : ""));
		cJSON_AddItemToArray(pair, cJSON_CreateNumber(schedule->weights[i]));
		cJSON_AddItemToArray(arr, pair);
	}
	return arr;
}

// Build seed JSON (int or weighted array)
static cJSON *build_seed_json(const struct daydream_seed_schedule *schedule)
{
	if (schedule->count <= 1) {
		int s = (schedule->count == 1) ? schedule->seeds[0] : 42;
		return cJSON_CreateNumber(s);
	}

	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < schedule->count; i++) {
		cJSON *pair = cJSON_CreateArray();
		cJSON_AddItemToArray(pair, cJSON_CreateNumber(schedule->seeds[i]));
		cJSON_AddItemToArray(pair, cJSON_CreateNumber(schedule->weights[i]));
		cJSON_AddItemToArray(arr, pair);
	}
	return arr;
}

// Build t_index_list JSON
static cJSON *build_step_schedule_json(const struct daydream_step_schedule *schedule)
{
	cJSON *arr = cJSON_CreateArray();
	if (schedule->count > 0) {
		for (int i = 0; i < schedule->count; i++) {
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(schedule->steps[i]));
		}
	} else {
		cJSON_AddItemToArray(arr, cJSON_CreateNumber(11));
	}
	return arr;
}

// Build IP Adapter JSON
static cJSON *build_ip_adapter_json(const struct daydream_ip_adapter_params *ip_adapter)
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddBoolToObject(obj, "enabled", ip_adapter->enabled);
	cJSON_AddNumberToObject(obj, "scale", ip_adapter->scale);
	cJSON_AddStringToObject(obj, "type", ip_adapter->type ? ip_adapter->type : "regular");
	return obj;
}

void daydream_api_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

void daydream_api_cleanup(void)
{
	curl_global_cleanup();
}

struct daydream_stream_result daydream_api_create_stream(const char *api_key,
							 const struct daydream_stream_params *params)
{
	struct daydream_stream_result result = {0};
	CURL *curl = NULL;
	struct curl_slist *headers = NULL;
	struct response_buffer response = {0};
	char *json_body = NULL;
	cJSON *root = NULL;
	cJSON *response_json = NULL;

	curl = curl_easy_init();
	if (!curl) {
		result.error = DAYDREAM_ERR_CURL_INIT;
		return result;
	}

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "x-client-source: obs");

	// Build JSON using cJSON
	root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "pipeline", "streamdiffusion");

	cJSON *json_params = cJSON_AddObjectToObject(root, "params");
	const char *model = params->model_id ? params->model_id : "stabilityai/sdxl-turbo";

	cJSON_AddStringToObject(json_params, "model_id", model);
	cJSON_AddItemToObject(json_params, "prompt", build_prompt_json(&params->prompt_schedule));
	cJSON_AddStringToObject(json_params, "negative_prompt", params->negative_prompt ? params->negative_prompt : "");
	cJSON_AddNumberToObject(json_params, "guidance_scale", params->guidance);
	cJSON_AddNumberToObject(json_params, "delta", params->delta);
	cJSON_AddNumberToObject(json_params, "num_inference_steps", params->num_inference_steps);
	cJSON_AddItemToObject(json_params, "t_index_list", build_step_schedule_json(&params->step_schedule));
	cJSON_AddNumberToObject(json_params, "width", params->width);
	cJSON_AddNumberToObject(json_params, "height", params->height);
	cJSON_AddBoolToObject(json_params, "do_add_noise", params->do_add_noise);
	cJSON_AddItemToObject(json_params, "seed", build_seed_json(&params->seed_schedule));
	cJSON_AddItemToObject(json_params, "ip_adapter", build_ip_adapter_json(&params->ip_adapter));

	if (params->ip_adapter.style_image_url && strlen(params->ip_adapter.style_image_url) > 0) {
		cJSON_AddStringToObject(json_params, "ip_adapter_style_image_url", params->ip_adapter.style_image_url);
	}

	// Interpolation settings
	if (params->prompt_schedule.count > 1) {
		cJSON_AddStringToObject(json_params, "prompt_interpolation_method",
					params->prompt_interpolation_method ? params->prompt_interpolation_method
									    : "slerp");
		cJSON_AddBoolToObject(json_params, "normalize_prompt_weights", params->normalize_prompt_weights);
	}
	if (params->seed_schedule.count > 1) {
		cJSON_AddStringToObject(json_params, "seed_interpolation_method",
					params->seed_interpolation_method ? params->seed_interpolation_method
									  : "slerp");
		cJSON_AddBoolToObject(json_params, "normalize_seed_weights", params->normalize_seed_weights);
	}

	cJSON_AddItemToObject(json_params, "controlnets", build_controlnets_json(model, &params->controlnets));

	json_body = cJSON_PrintUnformatted(root);
	if (!json_body) {
		result.error = DAYDREAM_ERR_JSON_SERIALIZE;
		goto cleanup;
	}

	char url[256];
	snprintf(url, sizeof(url), "%s/streams", DAYDREAM_API_BASE);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	blog(LOG_INFO, "[Daydream] Creating stream with model: %s", model);
	blog(LOG_INFO, "[Daydream] Prompt schedule count: %d", params->prompt_schedule.count);
	if (params->prompt_schedule.count > 0 && params->prompt_schedule.prompts[0]) {
		blog(LOG_INFO, "[Daydream] First prompt: %s", params->prompt_schedule.prompts[0]);
	}

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		result.error = DAYDREAM_ERR_CURL_PERFORM;
		result.error_detail = strdup(curl_easy_strerror(res));
		blog(LOG_ERROR, "[Daydream] API request failed: %s", result.error_detail);
		goto cleanup;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (http_code != 200 && http_code != 201) {
		result.error = daydream_error_from_http(http_code);
		if (response.data)
			result.error_detail = strdup(response.data);
		blog(LOG_ERROR, "[Daydream] API error (HTTP %ld): %s", http_code,
		     result.error_detail ? result.error_detail : "No response");
		goto cleanup;
	}

	blog(LOG_INFO, "[Daydream] API response: %s", response.data);

	// Parse response using cJSON
	response_json = cJSON_Parse(response.data);
	if (!response_json) {
		result.error = DAYDREAM_ERR_JSON_PARSE;
		blog(LOG_ERROR, "[Daydream] Failed to parse JSON response");
		goto cleanup;
	}

	result.stream_id = json_get_string(response_json, "id");
	result.whip_url = json_get_string(response_json, "whip_url");

	if (result.stream_id && result.whip_url) {
		result.error = DAYDREAM_OK;
		blog(LOG_INFO, "[Daydream] Stream created: %s", result.stream_id);
		blog(LOG_INFO, "[Daydream] WHIP URL: %s", result.whip_url);
	} else {
		result.error = DAYDREAM_ERR_JSON_MISSING_FIELD;
		blog(LOG_ERROR, "[Daydream] Missing id or whip_url in response");
	}

cleanup:
	if (curl)
		curl_easy_cleanup(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (root)
		cJSON_Delete(root);
	if (json_body)
		cJSON_free(json_body);
	if (response_json)
		cJSON_Delete(response_json);
	if (response.data)
		free(response.data);

	return result;
}

daydream_error_t daydream_api_update_stream(const char *api_key, const char *stream_id,
					    const struct daydream_stream_params *params, uint64_t update_flags)
{
	if (!api_key || !stream_id || !params || update_flags == 0)
		return DAYDREAM_ERR_NULL_PARAM;

	CURL *curl = NULL;
	struct curl_slist *headers = NULL;
	struct response_buffer response = {0};
	char *json_body = NULL;
	cJSON *root = NULL;
	daydream_error_t result = DAYDREAM_ERR_UNKNOWN;

	curl = curl_easy_init();
	if (!curl) {
		blog(LOG_ERROR, "[Daydream] Failed to initialize curl for update");
		return DAYDREAM_ERR_CURL_INIT;
	}

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "x-client-source: obs");

	// Build JSON using cJSON
	root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "pipeline", "streamdiffusion");

	cJSON *json_params = cJSON_AddObjectToObject(root, "params");
	const char *model = params->model_id ? params->model_id : "stabilityai/sdxl-turbo";

	// Always include model_id (required by API)
	cJSON_AddStringToObject(json_params, "model_id", model);

	// Prompt schedule
	if (update_flags & UPDATE_FLAG_PROMPT) {
		cJSON_AddItemToObject(json_params, "prompt", build_prompt_json(&params->prompt_schedule));
	}

	// Negative prompt
	if (update_flags & UPDATE_FLAG_NEGATIVE_PROMPT) {
		cJSON_AddStringToObject(json_params, "negative_prompt",
					params->negative_prompt ? params->negative_prompt : "");
	}

	// Seed schedule
	if (update_flags & UPDATE_FLAG_SEED) {
		cJSON_AddItemToObject(json_params, "seed", build_seed_json(&params->seed_schedule));
	}

	// Step schedule
	if (update_flags & UPDATE_FLAG_STEP_SCHEDULE) {
		cJSON_AddItemToObject(json_params, "t_index_list", build_step_schedule_json(&params->step_schedule));
	}

	// Guidance
	if (update_flags & UPDATE_FLAG_GUIDANCE) {
		cJSON_AddNumberToObject(json_params, "guidance_scale", params->guidance);
	}

	// Delta
	if (update_flags & UPDATE_FLAG_DELTA) {
		cJSON_AddNumberToObject(json_params, "delta", params->delta);
	}

	// ControlNets
	if (update_flags & UPDATE_FLAG_CONTROLNETS) {
		cJSON_AddItemToObject(json_params, "controlnets", build_controlnets_json(model, &params->controlnets));
	}

	// IP Adapter
	if (update_flags & UPDATE_FLAG_IP_ADAPTER) {
		cJSON_AddItemToObject(json_params, "ip_adapter", build_ip_adapter_json(&params->ip_adapter));
		if (params->ip_adapter.style_image_url && strlen(params->ip_adapter.style_image_url) > 0) {
			cJSON_AddStringToObject(json_params, "ip_adapter_style_image_url",
						params->ip_adapter.style_image_url);
		}
	}

	// Interpolation settings
	if (update_flags & UPDATE_FLAG_INTERP) {
		if (params->prompt_schedule.count > 1) {
			cJSON_AddStringToObject(
				json_params, "prompt_interpolation_method",
				params->prompt_interpolation_method ? params->prompt_interpolation_method : "slerp");
			cJSON_AddBoolToObject(json_params, "normalize_prompt_weights",
					      params->normalize_prompt_weights);
		}
		if (params->seed_schedule.count > 1) {
			cJSON_AddStringToObject(json_params, "seed_interpolation_method",
						params->seed_interpolation_method ? params->seed_interpolation_method
										  : "slerp");
			cJSON_AddBoolToObject(json_params, "normalize_seed_weights", params->normalize_seed_weights);
		}
	}

	json_body = cJSON_PrintUnformatted(root);
	if (!json_body) {
		blog(LOG_ERROR, "[Daydream] Failed to serialize update JSON");
		result = DAYDREAM_ERR_JSON_SERIALIZE;
		goto cleanup;
	}

	char url[512];
	snprintf(url, sizeof(url), "%s/streams/%s", DAYDREAM_API_BASE, stream_id);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

	blog(LOG_INFO, "[Daydream] Updating stream %s with flags 0x%llx", stream_id, (unsigned long long)update_flags);
	blog(LOG_DEBUG, "[Daydream] Update JSON: %s", json_body);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		blog(LOG_ERROR, "[Daydream] Update request failed: %s", curl_easy_strerror(res));
		result = DAYDREAM_ERR_CURL_PERFORM;
		goto cleanup;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (http_code != 200 && http_code != 204) {
		blog(LOG_ERROR, "[Daydream] Update failed with HTTP %ld: %s", http_code,
		     response.data ? response.data : "No response");
		result = daydream_error_from_http(http_code);
		goto cleanup;
	}

	blog(LOG_INFO, "[Daydream] Stream parameters updated successfully");
	result = DAYDREAM_OK;

cleanup:
	if (curl)
		curl_easy_cleanup(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (root)
		cJSON_Delete(root);
	if (json_body)
		cJSON_free(json_body);
	if (response.data)
		free(response.data);

	return result;
}

void daydream_api_free_result(struct daydream_stream_result *result)
{
	if (result->stream_id) {
		free(result->stream_id);
		result->stream_id = NULL;
	}
	if (result->whip_url) {
		free(result->whip_url);
		result->whip_url = NULL;
	}
	if (result->whep_url) {
		free(result->whep_url);
		result->whep_url = NULL;
	}
	if (result->error_detail) {
		free(result->error_detail);
		result->error_detail = NULL;
	}
	result->error = DAYDREAM_OK;
}
