#include "codec_config.h"
#include "test_helpers.h"

/* The helper resolves video0.codec + rcMode into the backend codec +
 * RC enum.  Signature: (codec, mode, out_codec, out_rc_mode).  The
 * RC-mode enum is codec-dependent (CBR=3 both; VBR H265=4/H264=2;
 * AVBR H265=5/H264=0; QVBR H265=6/H264=1). */
int test_codec_config(void)
{
	int failures = 0;
	PAYLOAD_TYPE_E codec = PT_H264;
	int rc_mode = -1;

	/* H.265 (NULL codec defaults to HEVC). */
	CHECK("rc_h265_cbr",
		codec_config_resolve_codec_rc(NULL, "cbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 3);
	CHECK("rc_h265_vbr",
		codec_config_resolve_codec_rc("h265", "vbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 4);
	CHECK("rc_h265_avbr",
		codec_config_resolve_codec_rc("h265", "avbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 5);
	CHECK("rc_h265_qvbr",
		codec_config_resolve_codec_rc("h265", "qvbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 6);

	/* H.264 — same CBR enum, different VBR/AVBR/QVBR enums. */
	CHECK("rc_h264_cbr",
		codec_config_resolve_codec_rc("h264", "cbr", &codec, &rc_mode) == 0 &&
		codec == PT_H264 && rc_mode == 3);
	CHECK("rc_h264_vbr",
		codec_config_resolve_codec_rc("h264", "vbr", &codec, &rc_mode) == 0 &&
		codec == PT_H264 && rc_mode == 2);
	CHECK("rc_h264_avbr",
		codec_config_resolve_codec_rc("h264", "avbr", &codec, &rc_mode) == 0 &&
		codec == PT_H264 && rc_mode == 0);
	CHECK("rc_h264_qvbr",
		codec_config_resolve_codec_rc("h264", "qvbr", &codec, &rc_mode) == 0 &&
		codec == PT_H264 && rc_mode == 1);

	/* Error paths. */
	CHECK("rc_invalid_mode",
		codec_config_resolve_codec_rc("h265", "bogus", &codec, &rc_mode) != 0);
	CHECK("rc_invalid_codec",
		codec_config_resolve_codec_rc("bogus", "cbr", &codec, &rc_mode) != 0);
	CHECK("rc_null_mode",
		codec_config_resolve_codec_rc("h265", NULL, &codec, &rc_mode) != 0);
	CHECK("rc_null_out_codec",
		codec_config_resolve_codec_rc("h265", "cbr", NULL, &rc_mode) != 0);
	CHECK("rc_null_out_rc_mode",
		codec_config_resolve_codec_rc("h265", "cbr", &codec, NULL) != 0);

	return failures;
}
