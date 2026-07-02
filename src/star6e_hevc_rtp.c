#include "star6e_hevc_rtp.h"

#include "h26x_util.h"
#include "hevc_rtp.h"

#include <string.h>

static int star6e_hevc_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	return star6e_output_send_rtp_parts(opaque, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
}

size_t star6e_hevc_rtp_send_frame_codec(PAYLOAD_TYPE_E codec,
	const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats)
{
	size_t total_bytes = 0;

	if (!stream || !output || !rtp)
		return 0;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

	/* O33 (latency): drive the RTP timestamp from the encoder's HARDWARE PTS
	 * (i6_venc_pack.timestamp, microseconds) instead of a fixed per-frame tick
	 * increment. The fixed increment assumes a perfectly regular cadence; when
	 * VENC delivers frames with real inter-frame jitter (variable encode time,
	 * dropped/duplicated frames under load), a fixed tick desynchronizes the RTP
	 * clock from wall time and forces the RX jitter buffer to stay deeper to
	 * absorb the drift. A wall-clock-anchored timestamp lets the RX run a
	 * shallower buffer => lower display latency. [INFERRED gain is HIL-only;
	 * SIL verifies only that the HW PTS propagates correctly at 90 kHz.]
	 *
	 * Map us -> 90 kHz RTP ticks, anchored on the first frame's PTS so the wire
	 * timestamp starts at the random SSRC base (rtp->timestamp) and advances by
	 * exactly the real elapsed media time. PTS==0 (SDK didn't stamp) -> fall back
	 * to the fixed frame_ticks path below (zero behavior change). */
	int used_hw_pts = 0;
	if (stream->count > 0 && stream->packet[0].timestamp != 0) {
		unsigned long long pts_us = stream->packet[0].timestamp;
		if (rtp->pts_anchor_us == 0)
			rtp->pts_anchor_us = pts_us;     /* first frame: establish anchor */
		/* 90 kHz ticks since anchor: us * 90 / 1000 == us * 9 / 100 (no overflow
		 * for any realistic uptime; wraps naturally in uint32 like RTP wants). */
		unsigned long long delta_us = pts_us - rtp->pts_anchor_us;
		rtp->timestamp = rtp->ssrc_ts_base + (uint32_t)((delta_us * 9ULL) / 100ULL);
		used_hw_pts = 1;
	}

	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
		const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int nal_count = (pack->packNum > 0) ?
			(unsigned int)pack->packNum : 1;

		if (pack->packNum > 0 && nal_count > info_cap)
			nal_count = info_cap;
		if (!pack->data)
			continue;

		for (unsigned int k = 0; k < nal_count; ++k) {
			const uint8_t *data = NULL;
			size_t length = 0;
			const uint8_t *nal_ptr;
			size_t nal_len;
			uint8_t nal_type;
			int last_nal;

			if (pack->packNum > 0) {
				MI_U32 offset = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || offset >= pack->length ||
				    len > (pack->length - offset)) {
					continue;
				}
				data = pack->data + offset;
				length = len;
			} else {
				if (pack->length <= pack->offset)
					continue;
				data = pack->data + pack->offset;
				length = pack->length - pack->offset;
			}

			if (!data || length == 0)
				continue;

			nal_ptr = data;
			nal_len = length;
			h26x_util_strip_start_code(&nal_ptr, &nal_len);
			if (nal_len == 0)
				continue;

			if (codec == PT_H264) {
				nal_type = h26x_util_h264_nalu_type(nal_ptr, nal_len);
				if (pack->packNum > 0)
					nal_type = (uint8_t)pack->packetInfo[k].packType.h264Nalu;
			} else {
				nal_type = h26x_util_hevc_nalu_type(nal_ptr, nal_len);
				if (pack->packNum > 0)
					nal_type = (uint8_t)pack->packetInfo[k].packType.h265Nalu;
			}

			if (params)
				h26x_param_sets_update(params, codec, nal_type, nal_ptr, nal_len);

			/* O77 fix: the marker bit must fire on the pack the SDK flags
			 * as the frame's end (pack->endFrame), not merely the last pack
			 * in this GetStream batch. In frame-mode the two coincide (one
			 * GetStream == one full frame, endFrame set on the final pack);
			 * in slice-mode (byFrame=0) a batch can be a partial frame, so
			 * "last pack in batch" != "last pack of frame" and the old
			 * check would mark every partial batch's tail NAL, truncating
			 * the AU downstream (h26x_util_strip_start_code-adjacent
			 * receivers close on marker=1). See star6e_stream_has_frame_end
			 * (this file's sibling helper in star6e_runtime.c) for the same
			 * endFrame-based boundary test. */
			last_nal = pack->endFrame &&
				((pack->packNum > 0 && k == nal_count - 1) ||
				 (pack->packNum == 0));

			if (params) {
				total_bytes += h26x_rtp_prepend_param_sets(codec, params,
					nal_type, rtp,
					star6e_hevc_rtp_write, output, max_payload,
					stats);
			}

			total_bytes += h26x_rtp_send_nal(codec, nal_ptr, nal_len, rtp,
				star6e_hevc_rtp_write, output, last_nal, max_payload,
				stats);
		}
	}

	/* O33: only advance by the fixed tick when we did NOT anchor to HW PTS.
	 * When HW PTS drives the clock, rtp->timestamp is recomputed per frame above,
	 * so a += here would double-count. */
	if (!used_hw_pts)
		rtp->timestamp += frame_ticks;
	return total_bytes;
}

size_t star6e_hevc_rtp_send_frame(const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats)
{
	return star6e_hevc_rtp_send_frame_codec(PT_H265, stream, output, rtp,
		frame_ticks, params, max_payload, stats);
}
