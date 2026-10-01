#include <string.h>

#include "star6e.h"
#include "star6e_qp_bounds.h"
#include "test_helpers.h"

/* O129 backlog #6: the video0 QP bounds onto the SDK's RC params. CBR has P (MaxQp/MinQp) and I (MaxIQp/MinIQp)
 * bounds; VBR/AVBR only the I ones (include/star6e.h MI_VENC_Param*). 0 = leave the field as the SDK gave it. */
int test_star6e_qp_bounds(void)
{
	int failures = 0;
	MI_VENC_RcParam_t p;
	Star6eQpBounds all = { .min_qp = 20, .max_qp = 40, .min_iqp = 18, .max_iqp = 38 };
	Star6eQpBounds none = { 0 };
	Star6eQpBounds only_max = { .max_qp = 42 };

	memset(&p, 0, sizeof(p));
	p.stParamH264Cbr.u32MaxQp = 48; p.stParamH264Cbr.u32MinQp = 12;
	CHECK("qpb h264 cbr all applied", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264CBR, &p, &all) == 0);
	CHECK("qpb h264 cbr fields", p.stParamH264Cbr.u32MinQp == 20 && p.stParamH264Cbr.u32MaxQp == 40 &&
	      p.stParamH264Cbr.u32MinIQp == 18 && p.stParamH264Cbr.u32MaxIQp == 38);

	memset(&p, 0, sizeof(p));
	p.stParamH264Cbr.u32MaxQp = 48; p.stParamH264Cbr.u32MinQp = 12; p.stParamH264Cbr.u32MaxIQp = 47;
	CHECK("qpb zero leaves the sdk values", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264CBR, &p, &none) == 0 &&
	      p.stParamH264Cbr.u32MaxQp == 48 && p.stParamH264Cbr.u32MinQp == 12 && p.stParamH264Cbr.u32MaxIQp == 47);
	CHECK("qpb one bound only", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264CBR, &p, &only_max) == 0 &&
	      p.stParamH264Cbr.u32MaxQp == 42 && p.stParamH264Cbr.u32MinQp == 12);

	memset(&p, 0, sizeof(p));
	CHECK("qpb h265 cbr", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H265CBR, &p, &all) == 0 &&
	      p.stParamH265Cbr.u32MinQp == 20 && p.stParamH265Cbr.u32MaxIQp == 38);

	/* VBR / AVBR: only the I bounds exist; the two P bounds are reported as ignored */
	memset(&p, 0, sizeof(p));
	CHECK("qpb h264 vbr ignores P", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264VBR, &p, &all) == 2 &&
	      p.stParamH264VBR.u32MinIQP == 18 && p.stParamH264VBR.u32MaxIQp == 38);
	memset(&p, 0, sizeof(p));
	CHECK("qpb h265 avbr ignores P", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H265AVBR, &p, &all) == 2 &&
	      p.stParamH265Avbr.u32MinIQp == 18 && p.stParamH265Avbr.u32MaxIQp == 38);
	CHECK("qpb vbr with I only is complete", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264VBR, &p,
	      &(Star6eQpBounds){ .min_iqp = 22 }) == 0);

	CHECK("qpb unknown mode", star6e_qp_bounds_apply(99, &p, &all) == -1);
	CHECK("qpb null", star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264CBR, NULL, &all) == -1 &&
	      star6e_qp_bounds_apply(I6_VENC_RATEMODE_H264CBR, &p, NULL) == -1);
	return failures;
}
