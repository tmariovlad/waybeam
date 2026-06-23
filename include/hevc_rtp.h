#ifndef HEVC_RTP_H
#define HEVC_RTP_H

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t total_nals;
	uint32_t single_packets;
	uint32_t fu_packets;
	uint32_t rtp_packets;
	uint32_t rtp_payload_bytes;
} HevcRtpStats;

/** Send one NAL for the given codec: single-NAL packet when it fits, else
 *  FU-A fragments (HEVC type 49 / H.264 type 28).  NALs are never aggregated
 *  (no RTP Aggregation Packets) so the wire stays compatible with receivers
 *  that only implement single-NAL + FU-A.  If is_last is non-zero, the final
 *  packet carries the RTP marker bit. */
size_t h26x_rtp_send_nal(PAYLOAD_TYPE_E codec, const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats);

/** Send the param-set prepend NALs (HEVC VPS/SPS/PPS, H.264 SPS/PPS) for an
 *  IDR NAL type, each as its own packet.  No-op for non-IDR types. */
size_t h26x_rtp_prepend_param_sets(PAYLOAD_TYPE_E codec,
	const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats);

/** HEVC convenience wrapper for h26x_rtp_send_nal (codec = PT_H265). */
size_t hevc_rtp_send_nal(const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats);

/** HEVC convenience wrapper for h26x_rtp_prepend_param_sets (PT_H265). */
size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats);

#endif /* HEVC_RTP_H */
