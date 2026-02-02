#include "daydream-http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Internal response buffer for CURL callbacks
struct response_buffer {
	char *data;
	size_t size;
};

// CURL write callback
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

// Build common headers for API requests
static struct curl_slist *build_headers(const char *auth_token)
{
	struct curl_slist *headers = NULL;

	if (auth_token && auth_token[0] != '\0') {
		char auth_header[512];
		snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", auth_token);
		headers = curl_slist_append(headers, auth_header);
	}

	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "x-client-source: obs");

	return headers;
}

// Perform HTTP request (shared implementation for POST/PATCH)
static daydream_http_response_t perform_request(const char *method, const char *url, const char *json_body,
						const char *auth_token, long timeout_secs)
{
	daydream_http_response_t response = {0};
	CURL *curl = NULL;
	struct curl_slist *headers = NULL;
	struct response_buffer buffer = {0};

	curl = curl_easy_init();
	if (!curl) {
		response.error_msg = strdup("Failed to initialize CURL");
		return response;
	}

	headers = build_headers(auth_token);
	if (!headers) {
		response.error_msg = strdup("Failed to build headers");
		curl_easy_cleanup(curl);
		return response;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

	if (timeout_secs > 0) {
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_secs);
	}

	if (json_body) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	}

	// Set HTTP method
	if (strcmp(method, "PATCH") == 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	}
	// POST is the default when POSTFIELDS is set

	CURLcode res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		response.error_msg = strdup(curl_easy_strerror(res));
		response.status_code = 0;
	} else {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
		response.body = buffer.data;
		response.body_size = buffer.size;
		buffer.data = NULL; // Ownership transferred to response
	}

	// Cleanup
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	free(buffer.data); // Only frees if ownership wasn't transferred

	return response;
}

// POST implementation
static daydream_http_response_t curl_post(daydream_http_client_t *client, const char *url, const char *json_body,
					  const char *auth_token, long timeout_secs)
{
	(void)client; // CURL client has no instance state
	return perform_request("POST", url, json_body, auth_token, timeout_secs);
}

// PATCH implementation
static daydream_http_response_t curl_patch(daydream_http_client_t *client, const char *url, const char *json_body,
					   const char *auth_token, long timeout_secs)
{
	(void)client;
	return perform_request("PATCH", url, json_body, auth_token, timeout_secs);
}

// Destroy CURL client
static void curl_destroy(daydream_http_client_t *client)
{
	if (client) {
		free(client);
	}
}

// Create CURL-based HTTP client
daydream_http_client_t *daydream_http_client_curl_create(void)
{
	daydream_http_client_t *client = calloc(1, sizeof(daydream_http_client_t));
	if (!client)
		return NULL;

	client->impl = NULL; // CURL doesn't need persistent state
	client->post = curl_post;
	client->patch = curl_patch;
	client->destroy = curl_destroy;

	return client;
}

// Free response resources
void daydream_http_response_free(daydream_http_response_t *response)
{
	if (!response)
		return;

	free(response->body);
	response->body = NULL;
	response->body_size = 0;

	free(response->error_msg);
	response->error_msg = NULL;

	response->status_code = 0;
}

// Global init/cleanup
void daydream_http_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

void daydream_http_cleanup(void)
{
	curl_global_cleanup();
}
