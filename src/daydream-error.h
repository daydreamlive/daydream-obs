#pragma once

// Daydream error codes
// Used across all modules for consistent error handling
typedef enum {
	DAYDREAM_OK = 0,

	// General errors
	DAYDREAM_ERR_NULL_PARAM,
	DAYDREAM_ERR_ALLOC,

	// Network/CURL errors
	DAYDREAM_ERR_CURL_INIT,
	DAYDREAM_ERR_CURL_PERFORM,
	DAYDREAM_ERR_TIMEOUT,

	// HTTP errors
	DAYDREAM_ERR_HTTP_400,
	DAYDREAM_ERR_HTTP_401,
	DAYDREAM_ERR_HTTP_403,
	DAYDREAM_ERR_HTTP_404,
	DAYDREAM_ERR_HTTP_500,
	DAYDREAM_ERR_HTTP_OTHER,

	// JSON errors
	DAYDREAM_ERR_JSON_SERIALIZE,
	DAYDREAM_ERR_JSON_PARSE,
	DAYDREAM_ERR_JSON_MISSING_FIELD,

	// Auth errors
	DAYDREAM_ERR_AUTH_EXPIRED,
	DAYDREAM_ERR_AUTH_INVALID,

	// Encoder/Decoder errors
	DAYDREAM_ERR_CODEC_INIT,
	DAYDREAM_ERR_CODEC_ENCODE,
	DAYDREAM_ERR_CODEC_DECODE,

	// WebRTC errors
	DAYDREAM_ERR_WHIP_CONNECT,
	DAYDREAM_ERR_WHEP_CONNECT,

	DAYDREAM_ERR_UNKNOWN = 999
} daydream_error_t;

// Convert error code to human-readable string
static inline const char *daydream_error_string(daydream_error_t err)
{
	switch (err) {
	case DAYDREAM_OK:
		return "Success";
	case DAYDREAM_ERR_NULL_PARAM:
		return "Null parameter";
	case DAYDREAM_ERR_ALLOC:
		return "Memory allocation failed";
	case DAYDREAM_ERR_CURL_INIT:
		return "Failed to initialize CURL";
	case DAYDREAM_ERR_CURL_PERFORM:
		return "CURL request failed";
	case DAYDREAM_ERR_TIMEOUT:
		return "Request timed out";
	case DAYDREAM_ERR_HTTP_400:
		return "Bad request (HTTP 400)";
	case DAYDREAM_ERR_HTTP_401:
		return "Unauthorized (HTTP 401)";
	case DAYDREAM_ERR_HTTP_403:
		return "Forbidden (HTTP 403)";
	case DAYDREAM_ERR_HTTP_404:
		return "Not found (HTTP 404)";
	case DAYDREAM_ERR_HTTP_500:
		return "Server error (HTTP 500)";
	case DAYDREAM_ERR_HTTP_OTHER:
		return "HTTP error";
	case DAYDREAM_ERR_JSON_SERIALIZE:
		return "Failed to serialize JSON";
	case DAYDREAM_ERR_JSON_PARSE:
		return "Failed to parse JSON response";
	case DAYDREAM_ERR_JSON_MISSING_FIELD:
		return "Missing required field in response";
	case DAYDREAM_ERR_AUTH_EXPIRED:
		return "Authentication expired";
	case DAYDREAM_ERR_AUTH_INVALID:
		return "Invalid authentication";
	case DAYDREAM_ERR_CODEC_INIT:
		return "Failed to initialize codec";
	case DAYDREAM_ERR_CODEC_ENCODE:
		return "Encoding failed";
	case DAYDREAM_ERR_CODEC_DECODE:
		return "Decoding failed";
	case DAYDREAM_ERR_WHIP_CONNECT:
		return "WHIP connection failed";
	case DAYDREAM_ERR_WHEP_CONNECT:
		return "WHEP connection failed";
	default:
		return "Unknown error";
	}
}

// Helper to convert HTTP status code to error
static inline daydream_error_t daydream_error_from_http(long http_code)
{
	if (http_code >= 200 && http_code < 300)
		return DAYDREAM_OK;
	switch (http_code) {
	case 400:
		return DAYDREAM_ERR_HTTP_400;
	case 401:
		return DAYDREAM_ERR_HTTP_401;
	case 403:
		return DAYDREAM_ERR_HTTP_403;
	case 404:
		return DAYDREAM_ERR_HTTP_404;
	case 500:
		return DAYDREAM_ERR_HTTP_500;
	default:
		return DAYDREAM_ERR_HTTP_OTHER;
	}
}
