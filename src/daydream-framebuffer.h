#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Double-buffer for producer/consumer frame handling.
 *
 * This module provides a thread-safe double buffer for frame data,
 * allowing a producer to write frames while a consumer reads from
 * a separate buffer without blocking.
 *
 * Usage:
 * - Producer calls begin_write(), writes to returned buffer, then end_write()
 * - Consumer calls begin_read() to get buffer pointer, processes, then end_read()
 * - Internal synchronization ensures no overlap between producer and consumer
 */

struct daydream_framebuffer;

/**
 * Create a frame buffer.
 * @return New frame buffer instance, or NULL on allocation failure
 */
struct daydream_framebuffer *daydream_framebuffer_create(void);

/**
 * Destroy a frame buffer and free all resources.
 * @param fb Frame buffer to destroy (NULL-safe)
 */
void daydream_framebuffer_destroy(struct daydream_framebuffer *fb);

/**
 * Configure buffer dimensions. Allocates internal buffers.
 * Can be called multiple times to resize.
 *
 * @param fb Frame buffer instance
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @param bytes_per_pixel Bytes per pixel (e.g., 4 for BGRA)
 * @return true on success, false on allocation failure
 */
bool daydream_framebuffer_configure(struct daydream_framebuffer *fb, uint32_t width, uint32_t height,
				    uint32_t bytes_per_pixel);

/**
 * Begin writing a frame. Returns pointer to the write buffer.
 * Caller must call end_write() after writing data.
 *
 * Thread-safe: automatically selects buffer not being read.
 *
 * @param fb Frame buffer instance
 * @param linesize Output: bytes per row (may include padding)
 * @return Pointer to write buffer, or NULL if not configured
 */
uint8_t *daydream_framebuffer_begin_write(struct daydream_framebuffer *fb, uint32_t *linesize);

/**
 * Complete writing a frame. Marks buffer as ready for reading.
 * Signals any waiting consumers.
 *
 * @param fb Frame buffer instance
 */
void daydream_framebuffer_end_write(struct daydream_framebuffer *fb);

/**
 * Begin reading a frame. Returns pointer to the read buffer.
 * Caller must call end_read() after processing.
 *
 * @param fb Frame buffer instance
 * @param data Output: pointer to frame data
 * @param width Output: frame width
 * @param height Output: frame height
 * @param linesize Output: bytes per row
 * @return true if frame available, false otherwise
 */
bool daydream_framebuffer_begin_read(struct daydream_framebuffer *fb, uint8_t **data, uint32_t *width, uint32_t *height,
				     uint32_t *linesize);

/**
 * Complete reading a frame. Releases buffer for reuse.
 *
 * @param fb Frame buffer instance
 */
void daydream_framebuffer_end_read(struct daydream_framebuffer *fb);

/**
 * Wait for a frame to become available.
 *
 * @param fb Frame buffer instance
 * @param timeout_ms Maximum time to wait in milliseconds (0 = non-blocking)
 * @return true if frame available, false on timeout
 */
bool daydream_framebuffer_wait(struct daydream_framebuffer *fb, uint32_t timeout_ms);

/**
 * Signal all waiting threads to wake up.
 * Useful when shutting down to unblock waiters.
 *
 * @param fb Frame buffer instance
 */
void daydream_framebuffer_signal(struct daydream_framebuffer *fb);

/**
 * Check if a frame is ready to read (non-blocking).
 *
 * @param fb Frame buffer instance
 * @return true if frame available
 */
bool daydream_framebuffer_has_frame(struct daydream_framebuffer *fb);

/**
 * Get current buffer dimensions.
 *
 * @param fb Frame buffer instance
 * @param width Output: current width (optional, can be NULL)
 * @param height Output: current height (optional, can be NULL)
 */
void daydream_framebuffer_get_dimensions(struct daydream_framebuffer *fb, uint32_t *width, uint32_t *height);

/**
 * Statistics for monitoring buffer performance.
 */
struct daydream_framebuffer_stats {
	uint64_t frames_written;
	uint64_t frames_read;
	uint64_t frames_dropped; // Frames overwritten before being read
};

/**
 * Get buffer statistics.
 *
 * @param fb Frame buffer instance
 * @param stats Output: statistics structure
 */
void daydream_framebuffer_get_stats(struct daydream_framebuffer *fb, struct daydream_framebuffer_stats *stats);

/**
 * Reset buffer state (clears ready flag and statistics).
 * Does not free memory.
 *
 * @param fb Frame buffer instance
 */
void daydream_framebuffer_reset(struct daydream_framebuffer *fb);
