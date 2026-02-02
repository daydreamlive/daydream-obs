#include "daydream-framebuffer.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

struct daydream_framebuffer {
	// Double buffer storage
	uint8_t *buffers[2];
	size_t buffer_size;

	// Frame dimensions
	uint32_t width;
	uint32_t height;
	uint32_t bytes_per_pixel;
	uint32_t linesize;

	// Buffer indices
	int produce_idx;  // Buffer index producer is writing to
	int consume_idx;  // Buffer index consumer is reading (-1 if idle)
	bool frame_ready; // True if a frame is ready to read

	// Synchronization
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	// Statistics
	uint64_t frames_written;
	uint64_t frames_read;
	uint64_t frames_dropped;
};

struct daydream_framebuffer *daydream_framebuffer_create(void)
{
	struct daydream_framebuffer *fb = calloc(1, sizeof(struct daydream_framebuffer));
	if (!fb)
		return NULL;

	fb->produce_idx = 0;
	fb->consume_idx = -1;
	fb->frame_ready = false;

	if (pthread_mutex_init(&fb->mutex, NULL) != 0) {
		free(fb);
		return NULL;
	}

	if (pthread_cond_init(&fb->cond, NULL) != 0) {
		pthread_mutex_destroy(&fb->mutex);
		free(fb);
		return NULL;
	}

	return fb;
}

void daydream_framebuffer_destroy(struct daydream_framebuffer *fb)
{
	if (!fb)
		return;

	pthread_cond_destroy(&fb->cond);
	pthread_mutex_destroy(&fb->mutex);

	free(fb->buffers[0]);
	free(fb->buffers[1]);
	free(fb);
}

bool daydream_framebuffer_configure(struct daydream_framebuffer *fb, uint32_t width, uint32_t height,
				    uint32_t bytes_per_pixel)
{
	if (!fb || width == 0 || height == 0 || bytes_per_pixel == 0)
		return false;

	pthread_mutex_lock(&fb->mutex);

	// Check if resize is needed
	uint32_t linesize = width * bytes_per_pixel;
	size_t new_size = (size_t)linesize * height;

	if (new_size != fb->buffer_size) {
		// Allocate new buffers
		uint8_t *new_buf0 = malloc(new_size);
		uint8_t *new_buf1 = malloc(new_size);

		if (!new_buf0 || !new_buf1) {
			free(new_buf0);
			free(new_buf1);
			pthread_mutex_unlock(&fb->mutex);
			return false;
		}

		// Free old buffers
		free(fb->buffers[0]);
		free(fb->buffers[1]);

		fb->buffers[0] = new_buf0;
		fb->buffers[1] = new_buf1;
		fb->buffer_size = new_size;
	}

	fb->width = width;
	fb->height = height;
	fb->bytes_per_pixel = bytes_per_pixel;
	fb->linesize = linesize;

	// Reset state on reconfigure
	fb->frame_ready = false;
	fb->produce_idx = 0;
	fb->consume_idx = -1;

	pthread_mutex_unlock(&fb->mutex);
	return true;
}

uint8_t *daydream_framebuffer_begin_write(struct daydream_framebuffer *fb, uint32_t *linesize)
{
	if (!fb)
		return NULL;

	pthread_mutex_lock(&fb->mutex);

	if (!fb->buffers[0]) {
		pthread_mutex_unlock(&fb->mutex);
		if (linesize)
			*linesize = 0;
		return NULL;
	}

	// Select buffer that consumer isn't reading
	int write_idx = (fb->consume_idx == 0) ? 1 : 0;
	fb->produce_idx = write_idx;

	if (linesize)
		*linesize = fb->linesize;

	// Note: we keep mutex locked during write to prevent reconfigure
	// This is intentional - caller must call end_write() to unlock
	// Actually, let's release the lock and rely on produce_idx tracking
	pthread_mutex_unlock(&fb->mutex);

	return fb->buffers[write_idx];
}

void daydream_framebuffer_end_write(struct daydream_framebuffer *fb)
{
	if (!fb)
		return;

	pthread_mutex_lock(&fb->mutex);

	// Check if previous frame wasn't read
	if (fb->frame_ready) {
		fb->frames_dropped++;
	}

	fb->frame_ready = true;
	fb->frames_written++;

	// Signal waiters
	pthread_cond_broadcast(&fb->cond);

	pthread_mutex_unlock(&fb->mutex);
}

bool daydream_framebuffer_begin_read(struct daydream_framebuffer *fb, uint8_t **data, uint32_t *width, uint32_t *height,
				     uint32_t *linesize)
{
	if (!fb)
		return false;

	pthread_mutex_lock(&fb->mutex);

	if (!fb->frame_ready || !fb->buffers[0]) {
		pthread_mutex_unlock(&fb->mutex);
		return false;
	}

	// Take ownership of the produced buffer
	fb->consume_idx = fb->produce_idx;
	fb->frame_ready = false;

	if (data)
		*data = fb->buffers[fb->consume_idx];
	if (width)
		*width = fb->width;
	if (height)
		*height = fb->height;
	if (linesize)
		*linesize = fb->linesize;

	pthread_mutex_unlock(&fb->mutex);
	return true;
}

void daydream_framebuffer_end_read(struct daydream_framebuffer *fb)
{
	if (!fb)
		return;

	pthread_mutex_lock(&fb->mutex);

	fb->consume_idx = -1;
	fb->frames_read++;

	pthread_mutex_unlock(&fb->mutex);
}

bool daydream_framebuffer_wait(struct daydream_framebuffer *fb, uint32_t timeout_ms)
{
	if (!fb)
		return false;

	pthread_mutex_lock(&fb->mutex);

	if (fb->frame_ready) {
		pthread_mutex_unlock(&fb->mutex);
		return true;
	}

	if (timeout_ms == 0) {
		pthread_mutex_unlock(&fb->mutex);
		return false;
	}

	// Calculate absolute timeout
	struct timespec ts;
	struct timeval tv;
	gettimeofday(&tv, NULL);

	ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
	ts.tv_nsec = tv.tv_usec * 1000 + (timeout_ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}

	while (!fb->frame_ready) {
		int rc = pthread_cond_timedwait(&fb->cond, &fb->mutex, &ts);
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&fb->mutex);
			return false;
		}
	}

	pthread_mutex_unlock(&fb->mutex);
	return true;
}

void daydream_framebuffer_signal(struct daydream_framebuffer *fb)
{
	if (!fb)
		return;

	pthread_mutex_lock(&fb->mutex);
	pthread_cond_broadcast(&fb->cond);
	pthread_mutex_unlock(&fb->mutex);
}

bool daydream_framebuffer_has_frame(struct daydream_framebuffer *fb)
{
	if (!fb)
		return false;

	pthread_mutex_lock(&fb->mutex);
	bool ready = fb->frame_ready;
	pthread_mutex_unlock(&fb->mutex);

	return ready;
}

void daydream_framebuffer_get_dimensions(struct daydream_framebuffer *fb, uint32_t *width, uint32_t *height)
{
	if (!fb)
		return;

	pthread_mutex_lock(&fb->mutex);

	if (width)
		*width = fb->width;
	if (height)
		*height = fb->height;

	pthread_mutex_unlock(&fb->mutex);
}

void daydream_framebuffer_get_stats(struct daydream_framebuffer *fb, struct daydream_framebuffer_stats *stats)
{
	if (!fb || !stats)
		return;

	pthread_mutex_lock(&fb->mutex);

	stats->frames_written = fb->frames_written;
	stats->frames_read = fb->frames_read;
	stats->frames_dropped = fb->frames_dropped;

	pthread_mutex_unlock(&fb->mutex);
}

void daydream_framebuffer_reset(struct daydream_framebuffer *fb)
{
	if (!fb)
		return;

	pthread_mutex_lock(&fb->mutex);

	fb->frame_ready = false;
	fb->produce_idx = 0;
	fb->consume_idx = -1;
	fb->frames_written = 0;
	fb->frames_read = 0;
	fb->frames_dropped = 0;

	pthread_mutex_unlock(&fb->mutex);
}
