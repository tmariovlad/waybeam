#ifndef CODEC_CONFIG_H
#define CODEC_CONFIG_H

#include "codec_types.h"

/** Resolve a codec string ("h264"/"avc"/"264" or "h265"/"hevc"/"265") into
 *  PAYLOAD_TYPE_E.  NULL/empty defaults to PT_H265.  Returns -1 on unknown. */
int codec_config_resolve_codec(const char *codec, PAYLOAD_TYPE_E *out_codec);

/** Resolve the video0.codec + rcMode strings into the backend codec + RC enum.
 *  The RC-mode enum is codec-dependent (CBR=3 both; VBR H265=4/H264=2;
 *  AVBR H265=5/H264=0; QVBR H265=6/H264=1). */
int codec_config_resolve_codec_rc(const char *codec, const char *mode,
	PAYLOAD_TYPE_E *out_codec, int *out_rc_mode);

#endif /* CODEC_CONFIG_H */
