#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Debounce tracker for coalescing rapid changes.
 *
 * This module provides a time-based debounce mechanism that accumulates
 * change flags over a configurable delay period. Multiple rapid changes
 * are coalesced into a single update.
 *
 * Usage:
 * 1. Create tracker with desired delay
 * 2. Call schedule() when changes occur (flags are OR'd together)
 * 3. Call check() periodically to see if delay has passed
 * 4. Call consume() to get flags and reset for next batch
 */

struct daydream_debounce;

/**
 * Time provider function type.
 * Returns current time in nanoseconds.
 * Allows injecting mock time for testing.
 */
typedef uint64_t (*daydream_time_fn)(void);

/**
 * Create a debounce tracker.
 *
 * @param delay_ns Debounce delay in nanoseconds
 * @param time_fn Time provider function (NULL uses real time)
 * @return New tracker instance, or NULL on allocation failure
 */
struct daydream_debounce *daydream_debounce_create(uint64_t delay_ns, daydream_time_fn time_fn);

/**
 * Destroy a debounce tracker.
 *
 * @param db Tracker to destroy (NULL-safe)
 */
void daydream_debounce_destroy(struct daydream_debounce *db);

/**
 * Schedule an update with the given flags.
 * Flags are OR'd with any pending flags.
 * Resets the delay timer.
 *
 * @param db Tracker instance
 * @param flags Flags to add to pending updates
 */
void daydream_debounce_schedule(struct daydream_debounce *db, uint64_t flags);

/**
 * Check if debounce delay has passed since last schedule.
 *
 * @param db Tracker instance
 * @return true if delay passed and flags are pending
 */
bool daydream_debounce_ready(struct daydream_debounce *db);

/**
 * Get pending flags and reset for next batch.
 * Returns 0 if no flags pending or delay hasn't passed.
 *
 * @param db Tracker instance
 * @return Accumulated flags, or 0
 */
uint64_t daydream_debounce_consume(struct daydream_debounce *db);

/**
 * Get pending flags without consuming.
 *
 * @param db Tracker instance
 * @return Current pending flags
 */
uint64_t daydream_debounce_peek(struct daydream_debounce *db);

/**
 * Check if any updates are pending (regardless of delay).
 *
 * @param db Tracker instance
 * @return true if flags are pending
 */
bool daydream_debounce_has_pending(struct daydream_debounce *db);

/**
 * Reset tracker state (clears all pending flags).
 *
 * @param db Tracker instance
 */
void daydream_debounce_reset(struct daydream_debounce *db);

/**
 * Get the configured delay in nanoseconds.
 *
 * @param db Tracker instance
 * @return Delay in nanoseconds
 */
uint64_t daydream_debounce_get_delay(struct daydream_debounce *db);

/**
 * Helper to safely compare strings (handles NULL).
 * Returns true if strings differ.
 *
 * @param old_str Previous string value (may be NULL)
 * @param new_str New string value (may be NULL)
 * @return true if strings are different
 */
bool daydream_str_changed(const char *old_str, const char *new_str);
