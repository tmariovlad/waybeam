#include "hevc_rtp.h"

static void apply_stats(HevcRtpStats *stats, const RtpPacketizerResult *result)
{
	if (!stats || !result || result->packet_count == 0)
		return;

	if (result->fragmented)
		stats->fu_packets += result->packet_count;
	else
		stats->single_packets += result->packet_count;
	stats->rtp_packets += result->packet_count;
	stats->rtp_payload_bytes += result->payload_bytes;
}

size_t h26x_rtp_send_nal(PAYLOAD_TYPE_E codec, const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerResult result;
	size_t total_bytes;

	if (!data || len == 0 || !rtp || !write_cb)
		return 0;

	if (stats)
		stats->total_nals++;

	if (codec == PT_H264)
		total_bytes = rtp_packetizer_send_h264_nal(rtp, write_cb, opaque,
			data, len, is_last, max_payload, stats ? &result : NULL);
	else
		total_bytes = rtp_packetizer_send_hevc_nal(rtp, write_cb, opaque,
			data, len, is_last, max_payload, stats ? &result : NULL);
	if (stats)
		apply_stats(stats, &result);
	return total_bytes;
}

size_t h26x_rtp_prepend_param_sets(PAYLOAD_TYPE_E codec,
	const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats)
{
	H26xParamSetRef refs[3];
	size_t count;
	size_t total_bytes = 0;
	size_t i;

	if (!params || !rtp)
		return 0;

	count = h26x_param_sets_get_prepend(params, codec, nal_type, refs,
		sizeof(refs) / sizeof(refs[0]));
	for (i = 0; i < count; ++i) {
		total_bytes += h26x_rtp_send_nal(codec, refs[i].data, refs[i].len,
			rtp, write_cb, opaque, 0, max_payload, stats);
	}

	return total_bytes;
}

size_t hevc_rtp_send_nal(const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats)
{
	return h26x_rtp_send_nal(PT_H265, data, len, rtp, write_cb, opaque,
		is_last, max_payload, stats);
}

size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats)
{
	return h26x_rtp_prepend_param_sets(PT_H265, params, nal_type, rtp,
		write_cb, opaque, max_payload, stats);
}
