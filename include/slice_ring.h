#ifndef SLICE_RING_H
#define SLICE_RING_H

/*
 * SPSC (Single-Producer Single-Consumer) bounded ring buffer for VENC
 * slice output, used to decouple the DRAIN thread (poll -> GetStream ->
 * ReleaseStream) from the SEND thread (packetize -> sendmsg) — see
 * repos/tasks/hil-build/HB-51-subframe-output-decouple-PLAN.md, Faza 1.
 *
 * The DRAIN thread is the sole producer: it copies each slice out of the
 * SDK-owned stream buffer (which is reused after MI_VENC_ReleaseStream)
 * into a pre-allocated ring slot, then pushes.  The SEND thread is the
 * sole consumer: it pops slices, packetizes, and performs the (possibly
 * blocking) network send.  Back-pressure from a slow network therefore
 * stalls only the SEND thread, never the DRAIN thread that keeps the
 * VENC hardware queue empty.
 *
 * In-process only (plain heap memory, not shared memory) — this is NOT
 * the cross-process venc_ring (see venc_ring.h), which moves RTP packets
 * from the venc process to a separate wfb_tx process over POSIX SHM.
 * slice_ring moves raw encoder slices between two threads of the same
 * process, one push/frame-boundary earlier in the pipeline.
 *
 * Concurrency: 1 producer, 1 consumer, no mutex on the hot path.  Index
 * variables use C11 stdatomic with acquire/release ordering, matching the
 * pattern already used by venc_ring.h (there via __atomic builtins; here
 * via <stdatomic.h> since there is no shared-memory ABI to keep stable).
 *
 * Overflow policy: when the ring is full, push() does NOT block and does
 * NOT overwrite in-flight slots.  It drops the incoming slice and, if the
 * slice is not itself a frame boundary, keeps dropping subsequent slices
 * until (and including) the next frame boundary.  This guarantees the
 * ring never holds a slice run that starts mid-frame, since a decoder can
 * do nothing useful with a partial frame missing its first slice(s).
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* One pre-allocated slot: fixed-size backing buffer + actual length. */
typedef struct {
	uint32_t len;         /* valid bytes in data[0..len) */
	uint8_t  is_frame_end; /* 1 if this slice carries the frame's last pack */
	uint8_t  _pad[3];
	uint8_t *data;         /* pointer into the ring's slot storage block */
} SliceRingSlot;

typedef struct {
	uint64_t pushed;   /* slices accepted into the ring */
	uint64_t popped;   /* slices consumed from the ring */
	uint64_t dropped;  /* slices rejected (ring full or mid-frame skip) */
} SliceRingStats;

typedef struct {
	SliceRingSlot *slots;        /* slot_count entries */
	uint8_t       *storage;      /* backing bytes for all slots, contiguous */
	uint32_t       slot_count;   /* capacity, must be > 0 */
	uint32_t       max_slice_bytes;

	/* Producer-owned write cursor, consumer-owned read cursor. Monotonic
	 * (never wrapped directly); slot index = cursor % slot_count. */
	_Atomic uint64_t write_idx;
	_Atomic uint64_t read_idx;

	/* Producer-local: once a full-ring drop happens mid-frame, keep
	 * dropping until the next frame boundary so the ring never holds a
	 * slice run with a missing head.  Only touched by the producer. */
	int in_drop_to_boundary;

	/* Stats are updated by their respective single owner thread only
	 * (pushed/dropped by producer, popped by consumer) but are read from
	 * either side for observability, so they are atomic too. */
	_Atomic uint64_t stat_pushed;
	_Atomic uint64_t stat_popped;
	_Atomic uint64_t stat_dropped;
} SliceRing;

/*
 * Initialize a ring with `capacity` slots, each able to hold up to
 * `max_slice_bytes` of slice data. Allocates all backing storage up
 * front (zero malloc on the push/pop hot path afterward).
 *
 * Returns 0 on success, -1 on invalid arguments or allocation failure.
 */
int slice_ring_init(SliceRing *ring, uint32_t capacity, uint32_t max_slice_bytes);

/*
 * Producer: push one slice into the ring, copying `len` bytes from `data`.
 * `is_frame_end` must be 1 if this slice carries the frame's last pack
 * (mirrors star6e_stream_has_frame_end() semantics), 0 for a partial
 * mid-frame slice.
 *
 * If the ring is full, the slice is dropped and, when the drop happens
 * mid-frame, the ring keeps dropping every subsequent push (even ones
 * that would otherwise fit) until a push with is_frame_end=1 completes
 * the resync — see slice_ring_drop_to_frame_boundary().
 *
 * If `len` exceeds max_slice_bytes, the slice is dropped (oversize is
 * treated the same as ring-full for boundary-drop purposes).
 *
 * Returns 0 if the slice was accepted, -1 if it was dropped.
 * Never blocks.
 */
int slice_ring_push(SliceRing *ring, const void *data, uint32_t len,
                     int is_frame_end);

/*
 * Consumer: pop one slice out of the ring into a caller-owned buffer.
 * `out_buf` must be at least `out_buf_size` bytes; the copied length is
 * written to `*out_len` and the frame-end flag to `*out_is_frame_end`.
 *
 * Returns 0 if a slice was popped, -1 if the ring is empty.
 * Never blocks.
 */
int slice_ring_pop(SliceRing *ring, void *out_buf, uint32_t out_buf_size,
                    uint32_t *out_len, int *out_is_frame_end);

/*
 * Producer-only: explicitly force the ring into "drop until next frame
 * boundary" state. Exposed so a caller that detects loss of sync by some
 * other means (e.g. a SDK-reported discontinuity) can request the same
 * resync-at-next-frame-start behavior that a full-ring drop triggers
 * automatically. Idempotent.
 */
void slice_ring_drop_to_frame_boundary(SliceRing *ring);

/*
 * Snapshot the lifetime counters. Safe to call from either thread.
 */
void slice_ring_stats(const SliceRing *ring, uint64_t *pushed,
                      uint64_t *popped, uint64_t *dropped);

/*
 * Release all backing storage. Not thread-safe with concurrent push/pop —
 * call only after both producer and consumer threads have stopped.
 */
void slice_ring_destroy(SliceRing *ring);

#endif /* SLICE_RING_H */
