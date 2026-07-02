/*
 * Unit tests for slice_ring.c — in-process SPSC bounded ring buffer for
 * VENC slice output (HB-51 Faza 1). Tests: lifecycle, single push/pop,
 * fill/drain, drop-at-frame-boundary on overflow, stats, and a concurrent
 * producer/consumer pthread stress test (safe under -fsanitize=thread).
 */

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "slice_ring.h"
#include "test_helpers.h"

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static int test_init_destroy(void)
{
	int failures = 0;

	SliceRing ring;
	int ret = slice_ring_init(&ring, 4, 1400);
	CHECK("init_ok", ret == 0);
	CHECK("init_slot_count", ring.slot_count == 4);
	CHECK("init_max_slice_bytes", ring.max_slice_bytes == 1400);
	CHECK("init_slots_alloc", ring.slots != NULL);
	CHECK("init_storage_alloc", ring.storage != NULL);
	slice_ring_destroy(&ring);

	/* Destroy NULL and destroy-after-destroy are safe */
	slice_ring_destroy(NULL);
	slice_ring_destroy(&ring);

	return failures;
}

static int test_init_validation(void)
{
	int failures = 0;
	SliceRing ring;

	CHECK("null_ring", slice_ring_init(NULL, 4, 1400) == -1);
	CHECK("zero_capacity", slice_ring_init(&ring, 0, 1400) == -1);
	CHECK("zero_max_bytes", slice_ring_init(&ring, 4, 0) == -1);

	return failures;
}

/* ── Single push/pop ────────────────────────────────────────────────── */

static int test_push_pop(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("pp_init", slice_ring_init(&ring, 4, 64) == 0);

	uint8_t payload[20];
	memset(payload, 0xAB, sizeof(payload));

	int ret = slice_ring_push(&ring, payload, sizeof(payload), 0);
	CHECK("pp_push_ok", ret == 0);

	uint8_t buf[64];
	uint32_t out_len = 0;
	int is_frame_end = -1;
	ret = slice_ring_pop(&ring, buf, sizeof(buf), &out_len, &is_frame_end);
	CHECK("pp_pop_ok", ret == 0);
	CHECK("pp_pop_len", out_len == sizeof(payload));
	CHECK("pp_pop_data", memcmp(buf, payload, sizeof(payload)) == 0);
	CHECK("pp_pop_not_frame_end", is_frame_end == 0);

	/* Ring should be empty now */
	ret = slice_ring_pop(&ring, buf, sizeof(buf), &out_len, &is_frame_end);
	CHECK("pp_empty", ret == -1);

	/* Frame-end flag round-trips */
	ret = slice_ring_push(&ring, payload, 4, 1);
	CHECK("pp_push_fe", ret == 0);
	ret = slice_ring_pop(&ring, buf, sizeof(buf), &out_len, &is_frame_end);
	CHECK("pp_pop_fe", ret == 0 && is_frame_end == 1);

	slice_ring_destroy(&ring);
	return failures;
}

static int test_pop_truncates_small_buffer(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("trunc_init", slice_ring_init(&ring, 2, 64) == 0);

	uint8_t payload[32];
	memset(payload, 0x5A, sizeof(payload));
	CHECK("trunc_push", slice_ring_push(&ring, payload, sizeof(payload), 0) == 0);

	uint8_t small_buf[8];
	uint32_t out_len = 0;
	int ret = slice_ring_pop(&ring, small_buf, sizeof(small_buf), &out_len, NULL);
	CHECK("trunc_pop_ok", ret == 0);
	CHECK("trunc_len_clamped", out_len == sizeof(small_buf));

	slice_ring_destroy(&ring);
	return failures;
}

/* ── Oversize push ──────────────────────────────────────────────────── */

static int test_push_oversize(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("of_init", slice_ring_init(&ring, 4, 32) == 0);

	uint8_t big[64];
	memset(big, 0xFF, sizeof(big));

	/* Exceeds max_slice_bytes -> dropped, and since not frame_end, ring
	 * enters drop-to-boundary. */
	int ret = slice_ring_push(&ring, big, 64, 0);
	CHECK("of_too_large_dropped", ret == -1);

	uint64_t pushed = 0, popped = 0, dropped = 0;
	slice_ring_stats(&ring, &pushed, &popped, &dropped);
	CHECK("of_dropped_count", dropped == 1);

	/* Still in resync: even a small, otherwise-valid push is dropped. */
	uint8_t small[8];
	ret = slice_ring_push(&ring, small, sizeof(small), 0);
	CHECK("of_resync_drops_next", ret == -1);

	/* A frame-end push clears the resync state. */
	ret = slice_ring_push(&ring, small, sizeof(small), 1);
	CHECK("of_resync_cleared_by_frame_end", ret == -1);

	/* Now a normal push succeeds again. */
	ret = slice_ring_push(&ring, small, sizeof(small), 0);
	CHECK("of_push_after_resync", ret == 0);

	/* Exactly at limit should succeed (fresh ring). */
	SliceRing ring2;
	CHECK("of_init2", slice_ring_init(&ring2, 4, 32) == 0);
	ret = slice_ring_push(&ring2, big, 32, 1);
	CHECK("of_exact_fit", ret == 0);

	slice_ring_destroy(&ring);
	slice_ring_destroy(&ring2);
	return failures;
}

/* ── Fill and drain ─────────────────────────────────────────────────── */

static int test_fill_drain(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("fd_init", slice_ring_init(&ring, 4, 32) == 0);

	uint8_t data[8];

	for (int i = 0; i < 4; i++) {
		memset(data, (uint8_t)i, sizeof(data));
		int ret = slice_ring_push(&ring, data, sizeof(data), 0);
		CHECK("fd_fill", ret == 0);
	}

	uint8_t buf[32];
	uint32_t out_len;
	for (int i = 0; i < 4; i++) {
		int ret = slice_ring_pop(&ring, buf, sizeof(buf), &out_len, NULL);
		CHECK("fd_drain_ok", ret == 0);
		CHECK("fd_drain_len", out_len == 8);
		CHECK("fd_drain_data", buf[0] == (uint8_t)i);
	}

	int ret = slice_ring_pop(&ring, buf, sizeof(buf), &out_len, NULL);
	CHECK("fd_empty", ret == -1);

	slice_ring_destroy(&ring);
	return failures;
}

/* ── Drop at frame boundary on ring-full ────────────────────────────── */

static int test_overflow_drops_to_frame_boundary(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("ovf_init", slice_ring_init(&ring, 2, 32) == 0);

	uint8_t data[8];
	memset(data, 0x11, sizeof(data));

	/* Fill the 2 slots with mid-frame slices (is_frame_end=0). */
	CHECK("ovf_fill0", slice_ring_push(&ring, data, sizeof(data), 0) == 0);
	CHECK("ovf_fill1", slice_ring_push(&ring, data, sizeof(data), 0) == 0);

	/* 3rd push: ring full, this slice is mid-frame -> dropped AND ring
	 * enters resync (because a partial run with a missing head would
	 * otherwise be left once older slots drain). */
	int ret = slice_ring_push(&ring, data, sizeof(data), 0);
	CHECK("ovf_full_drop", ret == -1);

	uint64_t pushed = 0, popped = 0, dropped = 0;
	slice_ring_stats(&ring, &pushed, &popped, &dropped);
	CHECK("ovf_pushed_count", pushed == 2);
	CHECK("ovf_dropped_count", dropped == 1);

	/* Drain the 2 slices that were already in the ring before the
	 * overflow — they are still valid (the drop only discarded the
	 * NEW slice, not existing slots). */
	uint8_t buf[32];
	uint32_t out_len;
	CHECK("ovf_drain0", slice_ring_pop(&ring, buf, sizeof(buf), &out_len, NULL) == 0);
	CHECK("ovf_drain1", slice_ring_pop(&ring, buf, sizeof(buf), &out_len, NULL) == 0);

	/* Ring is now empty, but producer is still resyncing: pushes are
	 * dropped until a frame-end slice passes, even though there is
	 * plenty of free capacity now. */
	ret = slice_ring_push(&ring, data, sizeof(data), 0);
	CHECK("ovf_resync_drops_despite_space", ret == -1);
	slice_ring_stats(&ring, &pushed, &popped, &dropped);
	CHECK("ovf_dropped_count_2", dropped == 2);

	/* The frame-end push that follows clears resync, but is itself the
	 * boundary marker being dropped (matches slice_ring_push()'s
	 * documented contract: the boundary push completes the resync,
	 * whether or not it was accepted). */
	ret = slice_ring_push(&ring, data, sizeof(data), 1);
	CHECK("ovf_resync_boundary_drop", ret == -1);

	/* Now pushes are accepted normally again. */
	ret = slice_ring_push(&ring, data, sizeof(data), 0);
	CHECK("ovf_push_after_resync", ret == 0);

	slice_ring_destroy(&ring);
	return failures;
}

static int test_explicit_drop_to_boundary(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("edb_init", slice_ring_init(&ring, 4, 32) == 0);

	uint8_t data[8];
	memset(data, 0x22, sizeof(data));

	slice_ring_drop_to_frame_boundary(&ring);
	int ret = slice_ring_push(&ring, data, sizeof(data), 0);
	CHECK("edb_dropped_mid_frame", ret == -1);
	ret = slice_ring_push(&ring, data, sizeof(data), 1);
	CHECK("edb_dropped_at_boundary", ret == -1);
	ret = slice_ring_push(&ring, data, sizeof(data), 0);
	CHECK("edb_accepted_after_boundary", ret == 0);

	/* Idempotent: calling it twice does not break anything. */
	slice_ring_drop_to_frame_boundary(&ring);
	slice_ring_drop_to_frame_boundary(&ring);
	ret = slice_ring_push(&ring, data, sizeof(data), 1);
	CHECK("edb_still_resyncing", ret == -1);
	ret = slice_ring_push(&ring, data, sizeof(data), 0);
	CHECK("edb_resumed", ret == 0);

	slice_ring_destroy(&ring);
	return failures;
}

/* ── Stats ──────────────────────────────────────────────────────────── */

static int test_stats(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("stats_init", slice_ring_init(&ring, 2, 32) == 0);

	uint8_t data[8];
	memset(data, 0x42, sizeof(data));

	slice_ring_push(&ring, data, sizeof(data), 0);
	slice_ring_push(&ring, data, sizeof(data), 1);
	slice_ring_push(&ring, data, sizeof(data), 0); /* full -> dropped */

	uint64_t pushed = 0, popped = 0, dropped = 0;
	slice_ring_stats(&ring, &pushed, &popped, &dropped);
	CHECK("stats_pushed", pushed == 2);
	CHECK("stats_dropped", dropped == 1);

	uint8_t buf[32];
	uint32_t out_len;
	slice_ring_pop(&ring, buf, sizeof(buf), &out_len, NULL);
	slice_ring_pop(&ring, buf, sizeof(buf), &out_len, NULL);
	slice_ring_stats(&ring, &pushed, &popped, &dropped);
	CHECK("stats_popped", popped == 2);

	/* NULL out-params are tolerated individually. */
	slice_ring_stats(&ring, NULL, NULL, NULL);
	slice_ring_stats(NULL, &pushed, &popped, &dropped);

	slice_ring_destroy(&ring);
	return failures;
}

/* ── Concurrent producer/consumer (TSan-safe SPSC) ──────────────────── */

#define CONCURRENT_COUNT 20000
#define CONCURRENT_SLOTS 64
#define CONCURRENT_MAX_BYTES 32

typedef struct {
	SliceRing *ring;
	int errors;
	_Atomic int producer_done; /* set by producer just before it returns */
} ThreadArg;

/*
 * Producer never retries a slice: slice_ring_push() never blocks, and a
 * real DRAIN thread does not hold a slice back and re-offer it after a
 * drop — it calls ReleaseStream and moves on to the next GetStream, same
 * as here. This matches the ring's documented never-blocks contract; a
 * retry-the-same-slice loop would livelock forever whenever a drop
 * lands on a non-frame-end slice, since drop-to-boundary state would
 * then reject that exact retried slice on every attempt.
 */
static void *slice_producer_thread(void *arg)
{
	ThreadArg *ta = (ThreadArg *)arg;
	uint8_t data[8];
	for (int i = 0; i < CONCURRENT_COUNT; i++) {
		data[0] = (uint8_t)(i & 0xFF);
		data[1] = (uint8_t)((i >> 8) & 0xFF);
		int is_frame_end = ((i % 4) == 3) ? 1 : 0; /* every 4th slice ends a frame */
		slice_ring_push(ta->ring, data, sizeof(data), is_frame_end);
	}
	atomic_store_explicit(&ta->producer_done, 1, memory_order_release);
	return NULL;
}

/*
 * Consumer drains until the ring is observed empty AFTER the producer
 * has posted producer_done — checking producer_done first and re-
 * draining once more afterward avoids the race of stopping while a
 * final in-flight slice is still visible only to a stale empty read.
 * Sequence numbers must be strictly increasing (never repeated, never
 * regress) — that is what actually proves SPSC correctness (no
 * torn/duplicated/reordered slices); gaps from drops under transient
 * ring-full pressure are expected and are not an error.
 */
static void *slice_consumer_thread(void *arg)
{
	ThreadArg *ta = (ThreadArg *)arg;
	uint8_t buf[CONCURRENT_MAX_BYTES];
	uint32_t out_len;
	int last_seq = -1;
	for (;;) {
		int done = atomic_load_explicit(&ta->producer_done, memory_order_acquire);
		if (slice_ring_pop(ta->ring, buf, sizeof(buf), &out_len, NULL) != 0) {
			if (done)
				break; /* producer finished and ring is empty: nothing left, ever */
			usleep(1);
			continue;
		}
		int seq = buf[0] | (buf[1] << 8);
		if (seq <= last_seq)
			ta->errors++;
		last_seq = seq;
	}
	return NULL;
}

static int test_concurrent(void)
{
	int failures = 0;
	SliceRing ring;
	CHECK("conc_init",
	      slice_ring_init(&ring, CONCURRENT_SLOTS, CONCURRENT_MAX_BYTES) == 0);

	/* Ring is small relative to CONCURRENT_COUNT, so transient drops
	 * under scheduling pressure are expected, not a bug — this validates
	 * SPSC ordering correctness (no torn/duplicated/reordered slices)
	 * under real thread concurrency, tolerating the drop policy that is
	 * core to the ring's contract. The drop-at-frame-boundary policy
	 * itself is verified precisely (deterministically) in the
	 * single-threaded tests above. Both threads share one ThreadArg so
	 * the consumer can observe producer_done. */
	ThreadArg targ = {.ring = &ring, .errors = 0, .producer_done = 0};

	pthread_t prod, cons;
	pthread_create(&cons, NULL, slice_consumer_thread, &targ);
	pthread_create(&prod, NULL, slice_producer_thread, &targ);

	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	CHECK("conc_no_reorder_or_dup", targ.errors == 0);

	uint64_t pushed = 0, popped = 0, dropped = 0;
	slice_ring_stats(&ring, &pushed, &popped, &dropped);
	/* Every attempt is accounted for exactly once: accepted (pushed) or
	 * rejected (dropped) — mirrors the ring's push contract (never
	 * blocks, never silently discards without counting). */
	CHECK("conc_pushed_plus_dropped", pushed + dropped == CONCURRENT_COUNT);
	/* Every accepted slice is eventually consumed (consumer loop only
	 * exits once it has drained `pushed` slices or the producer is long
	 * done and the ring stays empty). */
	CHECK("conc_all_pushed_popped", popped == pushed);

	slice_ring_destroy(&ring);
	return failures;
}

/* ── Entry point ────────────────────────────────────────────────────── */

int test_slice_ring(void)
{
	int failures = 0;

	failures += test_init_destroy();
	failures += test_init_validation();
	failures += test_push_pop();
	failures += test_pop_truncates_small_buffer();
	failures += test_push_oversize();
	failures += test_fill_drain();
	failures += test_overflow_drops_to_frame_boundary();
	failures += test_explicit_drop_to_boundary();
	failures += test_stats();
	failures += test_concurrent();

	return failures;
}
