#include "star6e_qp_bounds.h"

#define SETQ(field, v) do { if ((v) != 0) (field) = (v); } while (0)

int star6e_qp_bounds_apply(int rate_mode, MI_VENC_RcParam_t *param, const Star6eQpBounds *b)
{
	int p_bounds;

	if (!param || !b)
		return -1;
	p_bounds = (b->min_qp != 0) + (b->max_qp != 0);
	switch (rate_mode) {
	case I6_VENC_RATEMODE_H264CBR:
		SETQ(param->stParamH264Cbr.u32MinQp, b->min_qp);
		SETQ(param->stParamH264Cbr.u32MaxQp, b->max_qp);
		SETQ(param->stParamH264Cbr.u32MinIQp, b->min_iqp);
		SETQ(param->stParamH264Cbr.u32MaxIQp, b->max_iqp);
		return 0;
	case I6_VENC_RATEMODE_H265CBR:
		SETQ(param->stParamH265Cbr.u32MinQp, b->min_qp);
		SETQ(param->stParamH265Cbr.u32MaxQp, b->max_qp);
		SETQ(param->stParamH265Cbr.u32MinIQp, b->min_iqp);
		SETQ(param->stParamH265Cbr.u32MaxIQp, b->max_iqp);
		return 0;
	case I6_VENC_RATEMODE_H264VBR:
		SETQ(param->stParamH264VBR.u32MinIQP, b->min_iqp);
		SETQ(param->stParamH264VBR.u32MaxIQp, b->max_iqp);
		return p_bounds;
	case I6_VENC_RATEMODE_H265VBR:
		SETQ(param->stParamH265Vbr.u32MinIQP, b->min_iqp);
		SETQ(param->stParamH265Vbr.u32MaxIQp, b->max_iqp);
		return p_bounds;
	case I6_VENC_RATEMODE_H264AVBR:
		SETQ(param->stParamH264Avbr.u32MinIQp, b->min_iqp);
		SETQ(param->stParamH264Avbr.u32MaxIQp, b->max_iqp);
		return p_bounds;
	case I6_VENC_RATEMODE_H265AVBR:
		SETQ(param->stParamH265Avbr.u32MinIQp, b->min_iqp);
		SETQ(param->stParamH265Avbr.u32MaxIQp, b->max_iqp);
		return p_bounds;
	default:
		return -1;
	}
}
