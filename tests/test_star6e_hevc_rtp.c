#include "star6e_hevc_rtp.h"

#include "test_helpers.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int create_udp_receiver(uint16_t *port)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
	int socket_handle;

	socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_handle < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(0);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(socket_handle, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(socket_handle);
		return -1;
	}
	if (getsockname(socket_handle, (struct sockaddr *)&addr, &addr_len) != 0) {
		close(socket_handle);
		return -1;
	}

	(void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	*port = ntohs(addr.sin_port);
	return socket_handle;
}

static int test_star6e_hevc_rtp_sends_idr_with_separate_param_sets(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	static const uint8_t idr[] = { 0x26, 0x01, 0xDD, 0xEE };
	Star6eOutputSetup setup;
	Star6eOutput output;
	H26xParamSets params = {0};
	RtpPacketizerState rtp = {
		.seq = 0x1234,
		.timestamp = 0x01020304,
		.ssrc = 0x05060708,
		.payload_type = 97,
	};
	Star6eHevcRtpStats stats = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t buf[64];
	char uri[64];
	size_t sent_bytes;
	size_t total_received = 0;
	unsigned int datagrams = 0;
	int saw_aggregation = 0;
	int last_marker = 0;
	uint16_t port;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e hevc rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e hevc rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e hevc rtp init", ret == 0);

	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&params, PT_H265, 34, pps, sizeof(pps));

	pack.data = (uint8_t *)idr;
	pack.length = sizeof(idr);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(idr);
	pack.packetInfo[0].packType.h265Nalu = 19;
	stream.count = 1;
	stream.packet = &pack;

	sent_bytes = star6e_hevc_rtp_send_frame(&stream, &output, &rtp, 3000,
		&params, 1400, &stats);
	CHECK("star6e hevc rtp sent bytes",
		sent_bytes == sizeof(vps) + sizeof(sps) + sizeof(pps) + sizeof(idr));
	/* VPS, SPS, PPS, IDR each go out as their own single-NAL packet —
	 * no aggregation packet. */
	CHECK("star6e hevc rtp stats",
		stats.total_nals == 4 &&
		stats.single_packets == 4 &&
		stats.fu_packets == 0 &&
		stats.rtp_packets == 4 &&
		stats.rtp_payload_bytes ==
			(sizeof(vps) + sizeof(sps) + sizeof(pps) + sizeof(idr)));
	CHECK("star6e hevc rtp state advanced",
		rtp.seq == 0x1238 && rtp.timestamp == 0x01020EBc);

	/* Drain the four expected datagrams; verify none is a type-48 AP
	 * packet and the last one carries the marker bit.  All packets were
	 * sent synchronously by send_frame, so a non-blocking probe is enough
	 * to assert no extra datagram follows. */
	while (datagrams < 4) {
		ssize_t received = recv(recv_socket, buf, sizeof(buf), 0);

		if (received < 12)
			break;
		datagrams++;
		total_received += (size_t)received;
		last_marker = (buf[1] & 0x80) ? 1 : 0;
		if (((buf[12] >> 1) & 0x3F) == 48)
			saw_aggregation = 1;
	}
	CHECK("star6e hevc rtp four datagrams", datagrams == 4);
	CHECK("star6e hevc rtp no extra datagram",
		recv(recv_socket, buf, sizeof(buf), MSG_DONTWAIT) < 0);
	CHECK("star6e hevc rtp total bytes",
		total_received == (size_t)(4 * 12) + stats.rtp_payload_bytes);
	CHECK("star6e hevc rtp no aggregation packet", saw_aggregation == 0);
	CHECK("star6e hevc rtp marker on last datagram", last_marker == 1);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

/* O33: when the encoder stamps a real HW PTS on the pack, the RTP timestamp
 * must be DERIVED from it (90 kHz, anchored on the first frame's PTS + the
 * random ssrc_ts_base), NOT advanced by the fixed frame_ticks. PTS==0 must keep
 * the old fixed-tick behavior bit-for-bit. */
static int test_star6e_hevc_rtp_hwpts_drives_timestamp(void)
{
	static const uint8_t idr[] = { 0x26, 0x01, 0xDD, 0xEE };
	H26xParamSets params = {0};
	Star6eHevcRtpStats stats = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	int failures = 0;

	/* ssrc_ts_base = 1000 (random RTP start). frame_ticks would be 3000/frame. */
	RtpPacketizerState rtp = {
		.seq = 0x10, .timestamp = 1000, .ssrc = 0xABCD,
		.payload_type = 97, .ssrc_ts_base = 1000, .pts_anchor_us = 0,
	};

	pack.data = (uint8_t *)idr;
	pack.length = sizeof(idr);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(idr);
	pack.packetInfo[0].packType.h265Nalu = 19;
	stream.count = 1;
	stream.packet = &pack;

	/* Capture-only write: no socket needed; we only assert on rtp.timestamp. */
	Star6eOutput sink = {0};
	(void)sink;

	/* --- Frame 1: PTS = 100000 us. Anchor is set here, so timestamp stays at
	 *     ssrc_ts_base (delta = 0). --- */
	pack.timestamp = 100000ULL;
	/* send via the codec entry so frame_ticks=3000 is in play but must be IGNORED */
	star6e_hevc_rtp_send_frame_codec(PT_H265, &stream, &sink, &rtp, 3000,
		&params, 1400, &stats);
	CHECK("O33 anchor set on first HW-PTS frame", rtp.pts_anchor_us == 100000ULL);
	CHECK("O33 frame1 ts == ssrc_ts_base (delta 0)", rtp.timestamp == 1000);

	/* --- Frame 2: PTS = 100000 + 8333 us (one 120fps frame). 8333us*9/100 =
	 *     749 ticks. timestamp must be base+749 = 1749, NOT base+3000. --- */
	pack.timestamp = 100000ULL + 8333ULL;
	star6e_hevc_rtp_send_frame_codec(PT_H265, &stream, &sink, &rtp, 3000,
		&params, 1400, &stats);
	CHECK("O33 frame2 ts from HW PTS (90kHz), not frame_ticks",
		rtp.timestamp == 1000 + ((8333ULL * 9ULL) / 100ULL));
	CHECK("O33 anchor unchanged after frame2", rtp.pts_anchor_us == 100000ULL);

	/* --- Frame 3: PTS = 0 (encoder didn't stamp). Must FALL BACK to fixed
	 *     frame_ticks: previous timestamp + 3000. --- */
	uint32_t before = rtp.timestamp;
	pack.timestamp = 0ULL;
	star6e_hevc_rtp_send_frame_codec(PT_H265, &stream, &sink, &rtp, 3000,
		&params, 1400, &stats);
	CHECK("O33 PTS==0 falls back to fixed frame_ticks",
		rtp.timestamp == before + 3000);

	return failures;
}

int test_star6e_hevc_rtp(void)
{
	int failures = 0;

	failures += test_star6e_hevc_rtp_sends_idr_with_separate_param_sets();
	failures += test_star6e_hevc_rtp_hwpts_drives_timestamp();
	return failures;
}
