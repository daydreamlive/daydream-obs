#include "daydream-debounce.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

// Default time provider
static uint64_t default_time_fn(void)
{
#ifdef _WIN32
	LARGE_INTEGER freq, counter;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
#endif
}

struct daydream_debounce {
	uint64_t delay_ns;
	uint64_t last_schedule_time;
	uint64_t pending_flags;
	bool has_pending;
	daydream_time_fn time_fn;
};

struct daydream_debounce *daydream_debounce_create(uint64_t delay_ns, daydream_time_fn time_fn)
{
	struct daydream_debounce *db = calloc(1, sizeof(struct daydream_debounce));
	if (!db)
		return NULL;

	db->delay_ns = delay_ns;
	db->time_fn = time_fn ? time_fn : default_time_fn;
	db->has_pending = false;
	db->pending_flags = 0;
	db->last_schedule_time = 0;

	return db;
}

void daydream_debounce_destroy(struct daydream_debounce *db)
{
	free(db);
}

void daydream_debounce_schedule(struct daydream_debounce *db, uint64_t flags)
{
	if (!db || flags == 0)
		return;

	db->pending_flags |= flags;
	db->has_pending = true;
	db->last_schedule_time = db->time_fn();
}

bool daydream_debounce_ready(struct daydream_debounce *db)
{
	if (!db || !db->has_pending)
		return false;

	uint64_t now = db->time_fn();
	uint64_t elapsed = now - db->last_schedule_time;

	return elapsed >= db->delay_ns;
}

uint64_t daydream_debounce_consume(struct daydream_debounce *db)
{
	if (!db || !db->has_pending)
		return 0;

	if (!daydream_debounce_ready(db))
		return 0;

	uint64_t flags = db->pending_flags;
	db->pending_flags = 0;
	db->has_pending = false;

	return flags;
}

uint64_t daydream_debounce_peek(struct daydream_debounce *db)
{
	return db ? db->pending_flags : 0;
}

bool daydream_debounce_has_pending(struct daydream_debounce *db)
{
	return db ? db->has_pending : false;
}

void daydream_debounce_reset(struct daydream_debounce *db)
{
	if (!db)
		return;

	db->pending_flags = 0;
	db->has_pending = false;
	db->last_schedule_time = 0;
}

uint64_t daydream_debounce_get_delay(struct daydream_debounce *db)
{
	return db ? db->delay_ns : 0;
}

bool daydream_str_changed(const char *old_str, const char *new_str)
{
	if (!old_str && !new_str)
		return false;
	if (!old_str || !new_str)
		return true;
	return strcmp(old_str, new_str) != 0;
}
