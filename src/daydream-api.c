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

	size_t json_size = 2048;
	json_body = malloc(json_size);
	if (!json_body) {
		result.error = strdup("Failed to allocate memory");
		goto cleanup;
	}

	snprintf(json_body, json_size,
		 "{"
		 "\"pipeline\":\"streamdiffusion\","
		 "\"params\":{"
		 "\"model_id\":\"%s\","
		 "\"prompt\":\"%s\","
		 "\"negative_prompt\":\"%s\","
		 "\"guidance_scale\":%.2f,"
		 "\"delta\":%.2f,"
		 "\"num_inference_steps\":%d,"
		 "\"width\":%d,"
		 "\"height\":%d"
		 "}"
		 "}",
		 params->model_id, params->prompt, params->negative_prompt, params->guidance, params->delta,
		 params->steps, params->width, params->height);

	char url[256];
	snprintf(url, sizeof(url), "%s/streams", DAYDREAM_API_BASE);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	blog(LOG_INFO, "[Daydream] Creating stream with model: %s", params->model_id);
	blog(LOG_INFO, "[Daydream] Prompt: %s", params->prompt);

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
