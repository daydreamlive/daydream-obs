#include "daydream-stream.h"
#include <stdlib.h>
#include <string.h>

struct daydream_stream {
	// Configuration
	struct daydream_stream_config config;
	daydream_stream_ops_t ops;

	// State
	daydream_stream_state_t state;
	char *error_message;
	char *stream_id;

	// Components (owned by this struct, created via ops)
	void *encoder;
	void *decoder;
	void *whip;
	void *whep;
};

const char *daydream_stream_state_string(daydream_stream_state_t state)
{
	switch (state) {
	case DAYDREAM_STREAM_IDLE:
		return "idle";
	case DAYDREAM_STREAM_STARTING:
		return "starting";
	case DAYDREAM_STREAM_RUNNING:
		return "running";
	case DAYDREAM_STREAM_STOPPING:
		return "stopping";
	case DAYDREAM_STREAM_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static void set_state(struct daydream_stream *stream, daydream_stream_state_t state, const char *error)
{
	stream->state = state;

	free(stream->error_message);
	stream->error_message = error ? strdup(error) : NULL;

	if (stream->ops.on_state_change) {
		stream->ops.on_state_change(state, error, stream->ops.userdata);
	}
}

static void cleanup_components(struct daydream_stream *stream)
{
	// Cleanup in reverse order of creation

	if (stream->whep) {
		if (stream->ops.whep_disconnect)
			stream->ops.whep_disconnect(stream->whep, stream->ops.userdata);
		if (stream->ops.whep_destroy)
			stream->ops.whep_destroy(stream->whep, stream->ops.userdata);
		stream->whep = NULL;
	}

	if (stream->whip) {
		if (stream->ops.whip_disconnect)
			stream->ops.whip_disconnect(stream->whip, stream->ops.userdata);
		if (stream->ops.whip_destroy)
			stream->ops.whip_destroy(stream->whip, stream->ops.userdata);
		stream->whip = NULL;
	}

	if (stream->decoder) {
		if (stream->ops.decoder_destroy)
			stream->ops.decoder_destroy(stream->decoder, stream->ops.userdata);
		stream->decoder = NULL;
	}

	if (stream->encoder) {
		if (stream->ops.encoder_destroy)
			stream->ops.encoder_destroy(stream->encoder, stream->ops.userdata);
		stream->encoder = NULL;
	}

	free(stream->stream_id);
	stream->stream_id = NULL;
}

struct daydream_stream *daydream_stream_create(const struct daydream_stream_config *config,
					       const daydream_stream_ops_t *ops)
{
	if (!config)
		return NULL;

	struct daydream_stream *stream = calloc(1, sizeof(struct daydream_stream));
	if (!stream)
		return NULL;

	stream->config = *config;
	if (ops)
		stream->ops = *ops;
	stream->state = DAYDREAM_STREAM_IDLE;

	return stream;
}

void daydream_stream_destroy(struct daydream_stream *stream)
{
	if (!stream)
		return;

	if (stream->state == DAYDREAM_STREAM_RUNNING || stream->state == DAYDREAM_STREAM_STARTING) {
		daydream_stream_stop(stream);
	}

	cleanup_components(stream);
	free(stream->error_message);
	free(stream);
}

bool daydream_stream_start(struct daydream_stream *stream, const char *api_key)
{
	if (!stream || !api_key)
		return false;

	// Check valid state transition
	if (stream->state != DAYDREAM_STREAM_IDLE && stream->state != DAYDREAM_STREAM_ERROR) {
		return false;
	}

	set_state(stream, DAYDREAM_STREAM_STARTING, NULL);

	// Step 1: Create stream via API
	struct daydream_stream_api_result api_result = {0};
	if (stream->ops.api_create_stream) {
		stream->ops.api_create_stream(api_key, stream->config.width, stream->config.height,
					      stream->ops.userdata, &api_result);

		if (!api_result.success) {
			set_state(stream, DAYDREAM_STREAM_ERROR,
				  api_result.error_message ? api_result.error_message : "API call failed");
			free(api_result.stream_id);
			free(api_result.whip_url);
			free(api_result.error_message);
			return false;
		}

		stream->stream_id = api_result.stream_id;
	}

	// Step 2: Create encoder
	if (stream->ops.encoder_create) {
		stream->encoder = stream->ops.encoder_create(stream->config.width, stream->config.height,
							     stream->config.fps, stream->config.bitrate,
							     stream->ops.userdata);

		if (!stream->encoder) {
			set_state(stream, DAYDREAM_STREAM_ERROR, "Failed to create encoder");
			free(api_result.whip_url);
			free(api_result.error_message);
			cleanup_components(stream);
			return false;
		}
	}

	// Step 3: Create decoder
	if (stream->ops.decoder_create) {
		stream->decoder =
			stream->ops.decoder_create(stream->config.width, stream->config.height, stream->ops.userdata);

		if (!stream->decoder) {
			set_state(stream, DAYDREAM_STREAM_ERROR, "Failed to create decoder");
			free(api_result.whip_url);
			free(api_result.error_message);
			cleanup_components(stream);
			return false;
		}
	}

	// Step 4: Create and connect WHIP
	if (stream->ops.whip_create && api_result.whip_url) {
		stream->whip = stream->ops.whip_create(api_result.whip_url, api_key, stream->ops.userdata);

		if (!stream->whip) {
			set_state(stream, DAYDREAM_STREAM_ERROR, "Failed to create WHIP");
			free(api_result.whip_url);
			free(api_result.error_message);
			cleanup_components(stream);
			return false;
		}

		if (stream->ops.whip_connect) {
			if (!stream->ops.whip_connect(stream->whip, stream->ops.userdata)) {
				set_state(stream, DAYDREAM_STREAM_ERROR, "WHIP connection failed");
				free(api_result.whip_url);
				free(api_result.error_message);
				cleanup_components(stream);
				return false;
			}
		}
	}

	// Step 5: Create and connect WHEP (optional)
	const char *whep_url = NULL;
	if (stream->whip && stream->ops.whip_get_whep_url) {
		whep_url = stream->ops.whip_get_whep_url(stream->whip, stream->ops.userdata);
	}

	if (whep_url && stream->ops.whep_create) {
		stream->whep = stream->ops.whep_create(whep_url, stream->ops.userdata);

		if (stream->whep && stream->ops.whep_connect) {
			// WHEP connection failure is not fatal
			stream->ops.whep_connect(stream->whep, stream->ops.userdata);
		}
	}

	free(api_result.whip_url);
	free(api_result.error_message);

	set_state(stream, DAYDREAM_STREAM_RUNNING, NULL);
	return true;
}

void daydream_stream_stop(struct daydream_stream *stream)
{
	if (!stream)
		return;

	// Only stop if actually running or starting
	if (stream->state != DAYDREAM_STREAM_RUNNING && stream->state != DAYDREAM_STREAM_STARTING &&
	    stream->state != DAYDREAM_STREAM_ERROR) {
		return;
	}

	set_state(stream, DAYDREAM_STREAM_STOPPING, NULL);

	cleanup_components(stream);

	set_state(stream, DAYDREAM_STREAM_IDLE, NULL);
}

daydream_stream_state_t daydream_stream_get_state(struct daydream_stream *stream)
{
	return stream ? stream->state : DAYDREAM_STREAM_IDLE;
}

const char *daydream_stream_get_error(struct daydream_stream *stream)
{
	return stream ? stream->error_message : NULL;
}

const char *daydream_stream_get_id(struct daydream_stream *stream)
{
	return stream ? stream->stream_id : NULL;
}

void *daydream_stream_get_encoder(struct daydream_stream *stream)
{
	return stream ? stream->encoder : NULL;
}

void *daydream_stream_get_decoder(struct daydream_stream *stream)
{
	return stream ? stream->decoder : NULL;
}

void *daydream_stream_get_whip(struct daydream_stream *stream)
{
	return stream ? stream->whip : NULL;
}

void *daydream_stream_get_whep(struct daydream_stream *stream)
{
	return stream ? stream->whep : NULL;
}
