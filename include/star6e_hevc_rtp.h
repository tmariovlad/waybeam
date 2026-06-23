#ifndef STAR6E_HEVC_RTP_H
#define STAR6E_HEVC_RTP_H

#include "h26x_param_sets.h"
#include "hevc_rtp.h"
#include "rtp_packetizer.h"
#include "star6e.h"
#include "star6e_output.h"

#include <stddef.h>
#include <stdint.h>

typedef HevcRtpStats Star6eHevcRtpStats;

/** Packetize and send one encoder frame as RTP packets for the given codec
 *  (PT_H265 = HEVC FU type 49, PT_H264 = H.264 FU type 28). */
size_t star6e_hevc_rtp_send_frame_codec(PAYLOAD_TYPE_E codec,
	const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats);

/** HEVC convenience wrapper for star6e_hevc_rtp_send_frame_codec (PT_H265). */
size_t star6e_hevc_rtp_send_frame(const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats);

#endif /* STAR6E_HEVC_RTP_H */
