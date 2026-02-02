#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Stream orchestrator for managing the lifecycle of a Daydream stream.
 *
 * This module coordinates the sequence of operations for starting and
 * stopping a stream: API calls, encoder/decoder creation, and WebRTC
 * connections. It uses dependency injection for external operations,
 * making it testable with mock implementations.
 *
 * State machine:
 *   IDLE -> STARTING -> RUNNING -> STOPPING -> IDLE
 *                  \-> ERROR (on failure)
 */

/**
 * Stream states
 */
typedef enum {
	DAYDREAM_STREAM_IDLE,     // Not streaming
	DAYDREAM_STREAM_STARTING, // Connecting (async)
	DAYDREAM_STREAM_RUNNING,  // Actively streaming
	DAYDREAM_STREAM_STOPPING, // Disconnecting
	DAYDREAM_STREAM_ERROR     // Failed, check error message
} daydream_stream_state_t;

/**
 * Get human-readable name for a stream state.
 */
const char *daydream_stream_state_string(daydream_stream_state_t state);

/**
 * Stream configuration
 */
struct daydream_stream_config {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t bitrate;
};

/**
 * Result from stream creation API call
 */
struct daydream_stream_api_result {
	bool success;
	char *stream_id;
	char *whip_url;
	char *error_message;
};

/**
 * Operations interface for dependency injection.
 * All function pointers are optional (NULL = no-op or default behavior).
 */
typedef struct daydream_stream_ops {
	/**
	 * Create stream via API. Must populate result.
	 * @param api_key API key for authentication
	 * @param width Stream width
	 * @param height Stream height
	 * @param userdata User context
	 * @param result Output: API result (caller must free strings)
	 */
	void (*api_create_stream)(const char *api_key, uint32_t width, uint32_t height, void *userdata,
				  struct daydream_stream_api_result *result);

	/**
	 * Create encoder.
	 * @return Encoder handle, or NULL on failure
	 */
	void *(*encoder_create)(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, void *userdata);
	void (*encoder_destroy)(void *encoder, void *userdata);

	/**
	 * Create decoder.
	 * @return Decoder handle, or NULL on failure
	 */
	void *(*decoder_create)(uint32_t width, uint32_t height, void *userdata);
	void (*decoder_destroy)(void *decoder, void *userdata);

	/**
	 * Create and connect WHIP.
	 * @return WHIP handle, or NULL on failure
	 */
	void *(*whip_create)(const char *url, const char *api_key, void *userdata);
	bool (*whip_connect)(void *whip, void *userdata);
	const char *(*whip_get_whep_url)(void *whip, void *userdata);
	void (*whip_disconnect)(void *whip, void *userdata);
	void (*whip_destroy)(void *whip, void *userdata);

	/**
	 * Create and connect WHEP (optional, for receiving processed frames).
	 * @return WHEP handle, or NULL on failure
	 */
	void *(*whep_create)(const char *url, void *userdata);
	bool (*whep_connect)(void *whep, void *userdata);
	void (*whep_disconnect)(void *whep, void *userdata);
	void (*whep_destroy)(void *whep, void *userdata);

	/**
	 * Called when stream state changes.
	 */
	void (*on_state_change)(daydream_stream_state_t state, const char *error, void *userdata);

	/**
	 * User context passed to all callbacks.
	 */
	void *userdata;
} daydream_stream_ops_t;

struct daydream_stream;

/**
 * Create a stream orchestrator.
 *
 * @param config Stream configuration (dimensions, fps, bitrate)
 * @param ops Operations callbacks (for dependency injection)
 * @return New stream instance, or NULL on allocation failure
 */
struct daydream_stream *daydream_stream_create(const struct daydream_stream_config *config,
					       const daydream_stream_ops_t *ops);

/**
 * Destroy a stream orchestrator.
 * Stops streaming if active.
 *
 * @param stream Stream to destroy (NULL-safe)
 */
void daydream_stream_destroy(struct daydream_stream *stream);

/**
 * Start streaming.
 * Performs the full startup sequence: API call, encoder, decoder, WHIP, WHEP.
 *
 * @param stream Stream instance
 * @param api_key API key for authentication
 * @return true if startup sequence completed successfully
 */
bool daydream_stream_start(struct daydream_stream *stream, const char *api_key);

/**
 * Stop streaming.
 * Performs cleanup in reverse order: WHEP, WHIP, decoder, encoder.
 *
 * @param stream Stream instance
 */
void daydream_stream_stop(struct daydream_stream *stream);

/**
 * Get current stream state.
 *
 * @param stream Stream instance
 * @return Current state
 */
daydream_stream_state_t daydream_stream_get_state(struct daydream_stream *stream);

/**
 * Get error message (when state is ERROR).
 *
 * @param stream Stream instance
 * @return Error message, or NULL if no error
 */
const char *daydream_stream_get_error(struct daydream_stream *stream);

/**
 * Get stream ID (after successful start).
 *
 * @param stream Stream instance
 * @return Stream ID, or NULL if not started
 */
const char *daydream_stream_get_id(struct daydream_stream *stream);

/**
 * Access created components (for frame processing).
 * These return the handles created by the ops callbacks.
 */
void *daydream_stream_get_encoder(struct daydream_stream *stream);
void *daydream_stream_get_decoder(struct daydream_stream *stream);
void *daydream_stream_get_whip(struct daydream_stream *stream);
void *daydream_stream_get_whep(struct daydream_stream *stream);
