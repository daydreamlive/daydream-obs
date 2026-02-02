#pragma once

#include <stddef.h>
#include <stdbool.h>

// HTTP response from a request
typedef struct {
	char *body;       // Response body (caller must free with daydream_http_response_free)
	size_t body_size; // Size of body in bytes
	long status_code; // HTTP status code (0 if request failed before getting response)
	char *error_msg;  // Error message if request failed (e.g., network error)
} daydream_http_response_t;

// Forward declaration
typedef struct daydream_http_client daydream_http_client_t;

// HTTP client interface (virtual table pattern)
struct daydream_http_client {
	void *impl; // Implementation-specific data

	// Perform a POST request with JSON body
	// auth_token: Bearer token value (without "Bearer " prefix), can be NULL
	daydream_http_response_t (*post)(daydream_http_client_t *client, const char *url, const char *json_body,
					 const char *auth_token, long timeout_secs);

	// Perform a PATCH request with JSON body
	daydream_http_response_t (*patch)(daydream_http_client_t *client, const char *url, const char *json_body,
					  const char *auth_token, long timeout_secs);

	// Destroy the client and free resources
	void (*destroy)(daydream_http_client_t *client);
};

// Create an HTTP client using CURL
// Returns NULL on failure
// Caller must call client->destroy(client) when done
daydream_http_client_t *daydream_http_client_curl_create(void);

// Free response body and error_msg
// Safe to call with zeroed response
void daydream_http_response_free(daydream_http_response_t *response);

// Initialize HTTP subsystem (call once at startup)
void daydream_http_init(void);

// Cleanup HTTP subsystem (call once at shutdown)
void daydream_http_cleanup(void);
