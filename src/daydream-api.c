#include "daydream-api.h"
#include <obs-module.h>
#include <curl/curl.h>
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

static char *find_json_string(const char *json, const char *key)
{
	char search[256];
	snprintf(search, sizeof(search), "\"%s\"", key);

	const char *pos = strstr(json, search);
	if (!pos)
		return NULL;

	pos = strchr(pos + strlen(search), ':');
	if (!pos)
		return NULL;

	while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t'))
		pos++;

	if (*pos != '"')
		return NULL;

	pos++;
	const char *end = strchr(pos, '"');
	if (!end)
		return NULL;

	size_t len = end - pos;
	char *result = malloc(len + 1);
	if (!result)
		return NULL;

	memcpy(result, pos, len);
	result[len] = 0;
	return result;
}

// Escape a string for JSON (handles quotes, backslashes, newlines, etc.)
// Returns allocated string that must be freed by caller
static char *json_escape_string(const char *str)
{
	if (!str)
		return strdup("");

	// Count how much space we need
	size_t len = 0;
	for (const char *p = str; *p; p++) {
		switch (*p) {
		case '"':
		case '\\':
		case '\n':
		case '\r':
		case '\t':
			len += 2;
			break;
		default:
			len += 1;
			break;
		}
	}

	char *escaped = malloc(len + 1);
	if (!escaped)
		return NULL;

	char *out = escaped;
	for (const char *p = str; *p; p++) {
		switch (*p) {
		case '"':
			*out++ = '\\';
			*out++ = '"';
			break;
		case '\\':
			*out++ = '\\';
			*out++ = '\\';
			break;
		case '\n':
			*out++ = '\\';
			*out++ = 'n';
			break;
		case '\r':
			*out++ = '\\';
			*out++ = 'r';
			break;
		case '\t':
			*out++ = '\\';
			*out++ = 't';
			break;
		default:
			*out++ = *p;
			break;
		}
	}
	*out = '\0';
	return escaped;
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

	curl = curl_easy_init();
	if (!curl) {
		result.error = strdup("Failed to initialize curl");
		return result;
	}

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "x-client-source: obs");

	size_t json_size = 8192;
	json_body = malloc(json_size);
	if (!json_body) {
		result.error = strdup("Failed to allocate memory");
		goto cleanup;
	}

	// Build ControlNets array based on model
	char controlnets_json[2048];
	const char *model = params->model_id ? params->model_id : "";

	if (strcmp(model, "stabilityai/sd-turbo") == 0) {
		// SD Turbo (SD2.1): openpose, hed, canny, depth, color
		snprintf(controlnets_json, sizeof(controlnets_json),
			 "["
			 "{\"model_id\":\"thibaud/controlnet-sd21-depth-diffusers\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"depth_tensorrt\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"thibaud/controlnet-sd21-canny-diffusers\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"canny\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"thibaud/controlnet-sd21-hed-diffusers\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"hed\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"thibaud/controlnet-sd21-openpose-diffusers\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"openpose\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"thibaud/controlnet-sd21-color-diffusers\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"passthrough\",\"preprocessor_params\":{},\"enabled\":true}"
			 "]",
			 params->controlnets.depth_scale, params->controlnets.canny_scale,
			 params->controlnets.hed_scale, params->controlnets.openpose_scale,
			 params->controlnets.color_scale);
	} else if (strcmp(model, "stabilityai/sdxl-turbo") == 0) {
		// SDXL Turbo: depth, canny, tile
		snprintf(controlnets_json, sizeof(controlnets_json),
			 "["
			 "{\"model_id\":\"xinsir/controlnet-depth-sdxl-1.0\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"depth_tensorrt\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"xinsir/controlnet-canny-sdxl-1.0\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"canny\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"xinsir/controlnet-tile-sdxl-1.0\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"feedback\",\"preprocessor_params\":{},\"enabled\":true}"
			 "]",
			 params->controlnets.depth_scale, params->controlnets.canny_scale,
			 params->controlnets.tile_scale);
	} else {
		// SD1.5 models (Dreamshaper 8, Openjourney v4): depth, canny, tile
		snprintf(controlnets_json, sizeof(controlnets_json),
			 "["
			 "{\"model_id\":\"lllyasviel/control_v11f1p_sd15_depth\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"depth_tensorrt\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"lllyasviel/control_v11p_sd15_canny\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"canny\",\"preprocessor_params\":{},\"enabled\":true},"
			 "{\"model_id\":\"lllyasviel/control_v11f1e_sd15_tile\",\"conditioning_scale\":%.2f,"
			 "\"preprocessor\":\"feedback\",\"preprocessor_params\":{},\"enabled\":true}"
			 "]",
			 params->controlnets.depth_scale, params->controlnets.canny_scale,
			 params->controlnets.tile_scale);
	}

	// Build IP Adapter JSON
	char ip_adapter_json[512];
	const char *ip_type = params->ip_adapter.type ? params->ip_adapter.type : "regular";
	snprintf(ip_adapter_json, sizeof(ip_adapter_json), "{\"enabled\":%s,\"scale\":%.2f,\"type\":\"%s\"}",
		 params->ip_adapter.enabled ? "true" : "false", params->ip_adapter.scale, ip_type);

	// Build style image URL part (empty string if not set)
	char style_image_json[512] = "";
	if (params->ip_adapter.style_image_url && strlen(params->ip_adapter.style_image_url) > 0) {
		snprintf(style_image_json, sizeof(style_image_json), ",\"ip_adapter_style_image_url\":\"%s\"",
			 params->ip_adapter.style_image_url);
	}

	// Build prompt (single string or weighted list)
	char prompt_json[2048];
	if (params->prompt_schedule.count <= 1) {
		const char *p = (params->prompt_schedule.count == 1 && params->prompt_schedule.prompts[0])
					? params->prompt_schedule.prompts[0]
					: "strawberry";
		snprintf(prompt_json, sizeof(prompt_json), "\"%s\"", p);
	} else {
		char *ptr = prompt_json;
		ptr += sprintf(ptr, "[");
		for (int i = 0; i < params->prompt_schedule.count; i++) {
			if (i > 0)
				ptr += sprintf(ptr, ",");
			ptr += sprintf(ptr, "[\"%s\",%.2f]",
				       params->prompt_schedule.prompts[i] ? params->prompt_schedule.prompts[i] : "",
				       params->prompt_schedule.weights[i]);
		}
		sprintf(ptr, "]");
	}

	// Build seed (single int or weighted list)
	char seed_json[512];
	if (params->seed_schedule.count <= 1) {
		int s = (params->seed_schedule.count == 1) ? params->seed_schedule.seeds[0] : 42;
		snprintf(seed_json, sizeof(seed_json), "%d", s);
	} else {
		char *ptr = seed_json;
		ptr += sprintf(ptr, "[");
		for (int i = 0; i < params->seed_schedule.count; i++) {
			if (i > 0)
				ptr += sprintf(ptr, ",");
			ptr += sprintf(ptr, "[%d,%.2f]", params->seed_schedule.seeds[i],
				       params->seed_schedule.weights[i]);
		}
		sprintf(ptr, "]");
	}

	// Build t_index_list (step schedule)
	char t_index_json[256];
	if (params->step_schedule.count > 0) {
		char *ptr = t_index_json;
		ptr += sprintf(ptr, "[");
		for (int i = 0; i < params->step_schedule.count; i++) {
			if (i > 0)
				ptr += sprintf(ptr, ",");
			ptr += sprintf(ptr, "%d", params->step_schedule.steps[i]);
		}
		sprintf(ptr, "]");
	} else {
		strcpy(t_index_json, "[11]");
	}

	const char *prompt_interp = params->prompt_interpolation_method ? params->prompt_interpolation_method : "slerp";
	const char *seed_interp = params->seed_interpolation_method ? params->seed_interpolation_method : "slerp";

	// Build params JSON - only include interpolation params if schedule has multiple items
	char interp_params_json[512] = "";
	if (params->prompt_schedule.count > 1) {
		snprintf(interp_params_json, sizeof(interp_params_json),
			 ",\"prompt_interpolation_method\":\"%s\",\"normalize_prompt_weights\":%s", prompt_interp,
			 params->normalize_prompt_weights ? "true" : "false");
	}

	char seed_interp_json[512] = "";
	if (params->seed_schedule.count > 1) {
		snprintf(seed_interp_json, sizeof(seed_interp_json),
			 ",\"seed_interpolation_method\":\"%s\",\"normalize_seed_weights\":%s", seed_interp,
			 params->normalize_seed_weights ? "true" : "false");
	}

	snprintf(json_body, json_size,
		 "{"
		 "\"pipeline\":\"streamdiffusion\","
		 "\"params\":{"
		 "\"model_id\":\"%s\","
		 "\"prompt\":%s,"
		 "\"negative_prompt\":\"%s\","
		 "\"guidance_scale\":%.2f,"
		 "\"delta\":%.2f,"
		 "\"num_inference_steps\":%d,"
		 "\"t_index_list\":%s,"
		 "\"width\":%d,"
		 "\"height\":%d,"
		 "\"do_add_noise\":%s,"
		 "\"seed\":%s,"
		 "\"ip_adapter\":%s%s%s%s,"
		 "\"controlnets\":%s"
		 "}"
		 "}",
		 params->model_id, prompt_json, params->negative_prompt, params->guidance, params->delta,
		 params->num_inference_steps, t_index_json, params->width, params->height,
		 params->do_add_noise ? "true" : "false", seed_json, ip_adapter_json, style_image_json,
		 interp_params_json, seed_interp_json, controlnets_json);

	char url[256];
	snprintf(url, sizeof(url), "%s/streams", DAYDREAM_API_BASE);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	blog(LOG_INFO, "[Daydream] Creating stream with model: %s", params->model_id);
	blog(LOG_INFO, "[Daydream] Prompt schedule count: %d", params->prompt_schedule.count);
	if (params->prompt_schedule.count > 0 && params->prompt_schedule.prompts[0]) {
		blog(LOG_INFO, "[Daydream] First prompt: %s", params->prompt_schedule.prompts[0]);
	}

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		result.error = strdup(curl_easy_strerror(res));
		blog(LOG_ERROR, "[Daydream] API request failed: %s", result.error);
		goto cleanup;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (http_code != 200 && http_code != 201) {
		char error_msg[512];
		snprintf(error_msg, sizeof(error_msg), "HTTP %ld: %s", http_code,
			 response.data ? response.data : "No response");
		result.error = strdup(error_msg);
		blog(LOG_ERROR, "[Daydream] API error: %s", result.error);
		goto cleanup;
	}

	blog(LOG_INFO, "[Daydream] API response: %s", response.data);

	result.stream_id = find_json_string(response.data, "id");
	result.whip_url = find_json_string(response.data, "whip_url");

	if (result.stream_id && result.whip_url) {
		result.success = true;
		blog(LOG_INFO, "[Daydream] Stream created: %s", result.stream_id);
		blog(LOG_INFO, "[Daydream] WHIP URL: %s", result.whip_url);
	} else {
		result.error = strdup("Failed to parse response");
		blog(LOG_ERROR, "[Daydream] %s", result.error);
	}

cleanup:
	if (curl)
		curl_easy_cleanup(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (json_body)
		free(json_body);
	if (response.data)
		free(response.data);

	return result;
}

bool daydream_api_update_stream(const char *api_key, const char *stream_id, const struct daydream_stream_params *params,
				uint64_t update_flags)
{
	if (!api_key || !stream_id || !params || update_flags == 0)
		return false;

	CURL *curl = NULL;
	struct curl_slist *headers = NULL;
	struct response_buffer response = {0};
	char *json_body = NULL;
	bool success = false;

	curl = curl_easy_init();
	if (!curl) {
		blog(LOG_ERROR, "[Daydream] Failed to initialize curl for update");
		return false;
	}

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "x-client-source: obs");

	size_t json_size = 8192;
	json_body = malloc(json_size);
	if (!json_body) {
		blog(LOG_ERROR, "[Daydream] Failed to allocate memory for update");
		goto cleanup;
	}

	// Build JSON body with only changed parameters
	// Use json_escape_string for all string values to handle special characters
	char *ptr = json_body;
	ptr += sprintf(ptr, "{\"pipeline\":\"streamdiffusion\",\"params\":{");

	// Always include model_id (required by API)
	ptr += sprintf(ptr, "\"model_id\":\"%s\"", params->model_id ? params->model_id : "stabilityai/sdxl-turbo");

	// Prompt schedule
	if (update_flags & UPDATE_FLAG_PROMPT) {
		if (params->prompt_schedule.count <= 1) {
			const char *p = (params->prompt_schedule.count == 1 && params->prompt_schedule.prompts[0])
						? params->prompt_schedule.prompts[0]
						: "strawberry";
			char *escaped = json_escape_string(p);
			ptr += sprintf(ptr, ",\"prompt\":\"%s\"", escaped);
			free(escaped);
		} else {
			ptr += sprintf(ptr, ",\"prompt\":[");
			for (int i = 0; i < params->prompt_schedule.count; i++) {
				if (i > 0)
					ptr += sprintf(ptr, ",");
				char *escaped = json_escape_string(
					params->prompt_schedule.prompts[i] ? params->prompt_schedule.prompts[i] : "");
				ptr += sprintf(ptr, "[\"%s\",%.2f]", escaped, params->prompt_schedule.weights[i]);
				free(escaped);
			}
			ptr += sprintf(ptr, "]");
		}
	}

	// Negative prompt
	if (update_flags & UPDATE_FLAG_NEGATIVE_PROMPT) {
		char *escaped = json_escape_string(params->negative_prompt ? params->negative_prompt : "");
		ptr += sprintf(ptr, ",\"negative_prompt\":\"%s\"", escaped);
		free(escaped);
	}

	// Seed schedule
	if (update_flags & UPDATE_FLAG_SEED) {
		if (params->seed_schedule.count <= 1) {
			int s = (params->seed_schedule.count == 1) ? params->seed_schedule.seeds[0] : 42;
			ptr += sprintf(ptr, ",\"seed\":%d", s);
		} else {
			ptr += sprintf(ptr, ",\"seed\":[");
			for (int i = 0; i < params->seed_schedule.count; i++) {
				if (i > 0)
					ptr += sprintf(ptr, ",");
				ptr += sprintf(ptr, "[%d,%.2f]", params->seed_schedule.seeds[i],
					       params->seed_schedule.weights[i]);
			}
			ptr += sprintf(ptr, "]");
		}
	}

	// Step schedule (t_index_list)
	if (update_flags & UPDATE_FLAG_STEP_SCHEDULE) {
		ptr += sprintf(ptr, ",\"t_index_list\":[");
		if (params->step_schedule.count > 0) {
			for (int i = 0; i < params->step_schedule.count; i++) {
				if (i > 0)
					ptr += sprintf(ptr, ",");
				ptr += sprintf(ptr, "%d", params->step_schedule.steps[i]);
			}
		} else {
			ptr += sprintf(ptr, "11");
		}
		ptr += sprintf(ptr, "]");
	}

	// Guidance
	if (update_flags & UPDATE_FLAG_GUIDANCE) {
		ptr += sprintf(ptr, ",\"guidance_scale\":%.2f", params->guidance);
	}

	// Delta
	if (update_flags & UPDATE_FLAG_DELTA) {
		ptr += sprintf(ptr, ",\"delta\":%.2f", params->delta);
	}

	// ControlNets
	if (update_flags & UPDATE_FLAG_CONTROLNETS) {
		const char *model = params->model_id ? params->model_id : "";

		if (strcmp(model, "stabilityai/sd-turbo") == 0) {
			ptr += sprintf(
				ptr,
				",\"controlnets\":["
				"{\"model_id\":\"thibaud/controlnet-sd21-depth-diffusers\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"depth_tensorrt\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"thibaud/controlnet-sd21-canny-diffusers\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"canny\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"thibaud/controlnet-sd21-hed-diffusers\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"hed\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"thibaud/controlnet-sd21-openpose-diffusers\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"openpose\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"thibaud/controlnet-sd21-color-diffusers\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"passthrough\",\"preprocessor_params\":{},\"enabled\":true}"
				"]",
				params->controlnets.depth_scale, params->controlnets.canny_scale,
				params->controlnets.hed_scale, params->controlnets.openpose_scale,
				params->controlnets.color_scale);
		} else if (strcmp(model, "stabilityai/sdxl-turbo") == 0) {
			ptr += sprintf(
				ptr,
				",\"controlnets\":["
				"{\"model_id\":\"xinsir/controlnet-depth-sdxl-1.0\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"depth_tensorrt\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"xinsir/controlnet-canny-sdxl-1.0\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"canny\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"xinsir/controlnet-tile-sdxl-1.0\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"feedback\",\"preprocessor_params\":{},\"enabled\":true}"
				"]",
				params->controlnets.depth_scale, params->controlnets.canny_scale,
				params->controlnets.tile_scale);
		} else {
			ptr += sprintf(
				ptr,
				",\"controlnets\":["
				"{\"model_id\":\"lllyasviel/control_v11f1p_sd15_depth\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"depth_tensorrt\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"lllyasviel/control_v11p_sd15_canny\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"canny\",\"preprocessor_params\":{},\"enabled\":true},"
				"{\"model_id\":\"lllyasviel/control_v11f1e_sd15_tile\",\"conditioning_scale\":%.2f,"
				"\"preprocessor\":\"feedback\",\"preprocessor_params\":{},\"enabled\":true}"
				"]",
				params->controlnets.depth_scale, params->controlnets.canny_scale,
				params->controlnets.tile_scale);
		}
	}

	// IP Adapter
	if (update_flags & UPDATE_FLAG_IP_ADAPTER) {
		const char *ip_type = params->ip_adapter.type ? params->ip_adapter.type : "regular";
		ptr += sprintf(ptr, ",\"ip_adapter\":{\"enabled\":%s,\"scale\":%.2f,\"type\":\"%s\"}",
			       params->ip_adapter.enabled ? "true" : "false", params->ip_adapter.scale, ip_type);

		if (params->ip_adapter.style_image_url && strlen(params->ip_adapter.style_image_url) > 0) {
			char *escaped = json_escape_string(params->ip_adapter.style_image_url);
			ptr += sprintf(ptr, ",\"ip_adapter_style_image_url\":\"%s\"", escaped);
			free(escaped);
		}
	}

	// Interpolation settings
	if (update_flags & UPDATE_FLAG_INTERP) {
		if (params->prompt_schedule.count > 1) {
			const char *prompt_interp =
				params->prompt_interpolation_method ? params->prompt_interpolation_method : "slerp";
			ptr += sprintf(ptr, ",\"prompt_interpolation_method\":\"%s\",\"normalize_prompt_weights\":%s",
				       prompt_interp, params->normalize_prompt_weights ? "true" : "false");
		}
		if (params->seed_schedule.count > 1) {
			const char *seed_interp = params->seed_interpolation_method ? params->seed_interpolation_method
										    : "slerp";
			ptr += sprintf(ptr, ",\"seed_interpolation_method\":\"%s\",\"normalize_seed_weights\":%s",
				       seed_interp, params->normalize_seed_weights ? "true" : "false");
		}
	}

	ptr += sprintf(ptr, "}}");

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
	blog(LOG_INFO, "[Daydream] Update JSON: %s", json_body);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		blog(LOG_ERROR, "[Daydream] Update request failed: %s", curl_easy_strerror(res));
		goto cleanup;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (http_code != 200 && http_code != 204) {
		blog(LOG_ERROR, "[Daydream] Update failed with HTTP %ld: %s", http_code,
		     response.data ? response.data : "No response");
		goto cleanup;
	}

	blog(LOG_INFO, "[Daydream] Stream parameters updated successfully");
	success = true;

cleanup:
	if (curl)
		curl_easy_cleanup(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (json_body)
		free(json_body);
	if (response.data)
		free(response.data);

	return success;
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
	if (result->error) {
		free(result->error);
		result->error = NULL;
	}
}
