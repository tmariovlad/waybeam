#include "codec_config.h"

#include <stdio.h>
#include <string.h>

int codec_config_resolve_codec(const char *codec, PAYLOAD_TYPE_E *out_codec)
{
	if (!out_codec) {
		fprintf(stderr, "ERROR: invalid codec arguments\n");
		return -1;
	}

	/* NULL/empty defaults to HEVC (back-compat with configs that predate
	 * the video0.codec key). */
	if (!codec || codec[0] == '\0' ||
	    !strcmp(codec, "h265") || !strcmp(codec, "hevc") ||
	    !strcmp(codec, "265")) {
		*out_codec = PT_H265;
		return 0;
	}
	if (!strcmp(codec, "h264") || !strcmp(codec, "avc") ||
	    !strcmp(codec, "264")) {
		*out_codec = PT_H264;
		return 0;
	}

	fprintf(stderr, "ERROR: unsupported video0.codec '%s'\n", codec);
	return -1;
}

int codec_config_resolve_codec_rc(const char *codec, const char *mode,
	PAYLOAD_TYPE_E *out_codec, int *out_rc_mode)
{
	PAYLOAD_TYPE_E c;

	if (!mode || !out_codec || !out_rc_mode) {
		fprintf(stderr, "ERROR: invalid rcMode arguments\n");
		return -1;
	}

	if (codec_config_resolve_codec(codec, &c) != 0)
		return -1;
	*out_codec = c;

	/* The backend RC-mode enum differs by codec — see fill_maruko_rc_attr()
	 * / star6e_pipeline_start_venc():
	 *   CBR  = 3 (both codecs)
	 *   VBR  : H265=4  H264=2
	 *   AVBR : H265=5  H264=0
	 *   QVBR : H265=6  H264=1 */
	if (!strcmp(mode, "cbr")) {
		*out_rc_mode = 3;
	} else if (!strcmp(mode, "vbr")) {
		*out_rc_mode = (c == PT_H264) ? 2 : 4;
	} else if (!strcmp(mode, "avbr")) {
		*out_rc_mode = (c == PT_H264) ? 0 : 5;
	} else if (!strcmp(mode, "qvbr")) {
		*out_rc_mode = (c == PT_H264) ? 1 : 6;
	} else {
		fprintf(stderr, "ERROR: unsupported video0.rcMode '%s'\n", mode);
		return -1;
	}
	return 0;
}
