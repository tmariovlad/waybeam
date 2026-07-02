#include "slice_ring.h"

#include <stdlib.h>
#include <string.h>

int slice_ring_init(SliceRing *ring, uint32_t capacity, uint32_t max_slice_bytes)
{
	if (!ring || capacity == 0 || max_slice_bytes == 0)
		return -1;

	memset(ring, 0, sizeof(*ring));

	ring->slots = (SliceRingSlot *)calloc(capacity, sizeof(SliceRingSlot));
	if (!ring->slots)
		return -1;

	ring->storage = (uint8_t *)malloc((size_t)capacity * max_slice_bytes);
	if (!ring->storage) {
		free(ring->slots);
		ring->slots = NULL;
		return -1;
	}

	for (uint32_t i = 0; i < capacity; i++)
		ring->slots[i].data = ring->storage + (size_t)i * max_slice_bytes;

	ring->slot_count = capacity;
	ring->max_slice_bytes = max_slice_bytes;
	atomic_store_explicit(&ring->write_idx, 0, memory_order_relaxed);
	atomic_store_explicit(&ring->read_idx, 0, memory_order_relaxed);
	ring->in_drop_to_boundary = 0;
	atomic_store_explicit(&ring->stat_pushed, 0, memory_order_relaxed);
	atomic_store_explicit(&ring->stat_popped, 0, memory_order_relaxed);
	atomic_store_explicit(&ring->stat_dropped, 0, memory_order_relaxed);

	return 0;
}

void slice_ring_drop_to_frame_boundary(SliceRing *ring)
{
	if (!ring)
		return;
	ring->in_drop_to_boundary = 1;
}

int slice_ring_push(SliceRing *ring, const void *data, uint32_t len,
                     int is_frame_end)
{
	if (!ring || !ring->slots || !data)
		return -1;

	/* Resync in progress: keep dropping until a frame boundary passes,
	 * regardless of whether this particular slice would otherwise fit. */
	if (ring->in_drop_to_boundary) {
		atomic_fetch_add_explicit(&ring->stat_dropped, 1, memory_order_relaxed);
		if (is_frame_end)
			ring->in_drop_to_boundary = 0;
		return -1;
	}

	if (len > ring->max_slice_bytes) {
		atomic_fetch_add_explicit(&ring->stat_dropped, 1, memory_order_relaxed);
		if (!is_frame_end)
			ring->in_drop_to_boundary = 1;
		return -1;
	}

	uint64_t w = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
	uint64_t r = atomic_load_explicit(&ring->read_idx, memory_order_acquire);

	if (w - r >= ring->slot_count) {
		/* Ring full: drop this slice. If it was not the frame's last
		 * slice, the ring would otherwise end up holding a run whose
		 * head is missing once older slots free up — force a resync
		 * to the next frame boundary instead. */
		atomic_fetch_add_explicit(&ring->stat_dropped, 1, memory_order_relaxed);
		if (!is_frame_end)
			ring->in_drop_to_boundary = 1;
		return -1;
	}

	uint32_t idx = (uint32_t)(w % ring->slot_count);
	SliceRingSlot *slot = &ring->slots[idx];
	memcpy(slot->data, data, len);
	slot->len = len;
	slot->is_frame_end = is_frame_end ? 1 : 0;

	atomic_store_explicit(&ring->write_idx, w + 1, memory_order_release);
	atomic_fetch_add_explicit(&ring->stat_pushed, 1, memory_order_relaxed);

	return 0;
}

int slice_ring_pop(SliceRing *ring, void *out_buf, uint32_t out_buf_size,
                    uint32_t *out_len, int *out_is_frame_end)
{
	if (!ring || !ring->slots || !out_buf)
		return -1;

	uint64_t r = atomic_load_explicit(&ring->read_idx, memory_order_relaxed);
	uint64_t w = atomic_load_explicit(&ring->write_idx, memory_order_acquire);

	if (r >= w)
		return -1; /* ring empty */

	uint32_t idx = (uint32_t)(r % ring->slot_count);
	SliceRingSlot *slot = &ring->slots[idx];

	uint32_t len = slot->len;
	if (len > out_buf_size)
		len = out_buf_size; /* truncate defensively; caller sized buffer wrong */
	memcpy(out_buf, slot->data, len);

	if (out_len)
		*out_len = len;
	if (out_is_frame_end)
		*out_is_frame_end = slot->is_frame_end;

	atomic_store_explicit(&ring->read_idx, r + 1, memory_order_release);
	atomic_fetch_add_explicit(&ring->stat_popped, 1, memory_order_relaxed);

	return 0;
}

void slice_ring_stats(const SliceRing *ring, uint64_t *pushed,
                      uint64_t *popped, uint64_t *dropped)
{
	if (!ring)
		return;
	if (pushed)
		*pushed = atomic_load_explicit(&ring->stat_pushed, memory_order_relaxed);
	if (popped)
		*popped = atomic_load_explicit(&ring->stat_popped, memory_order_relaxed);
	if (dropped)
		*dropped = atomic_load_explicit(&ring->stat_dropped, memory_order_relaxed);
}

void slice_ring_destroy(SliceRing *ring)
{
	if (!ring)
		return;
	free(ring->slots);
	free(ring->storage);
	ring->slots = NULL;
	ring->storage = NULL;
	ring->slot_count = 0;
}
