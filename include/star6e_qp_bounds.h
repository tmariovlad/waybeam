#ifndef STAR6E_QP_BOUNDS_H
#define STAR6E_QP_BOUNDS_H

#include <stdint.h>

#include "star6e.h"

/* O129 backlog #6: the video0 QP bounds, written onto the SDK's RC params. 0 = leave the field as the SDK gave it. */
typedef struct {
	uint32_t min_qp, max_qp;     /* P frames: only CBR has these fields on this SDK */
	uint32_t min_iqp, max_iqp;   /* I frames: CBR, VBR and AVBR */
} Star6eQpBounds;

/* Sets the non-zero bounds that rate_mode's param struct has. Returns -1 for an unknown mode or a NULL argument,
 * else the number of non-zero bounds the mode has no field for (ignored; P bounds under VBR/AVBR). */
int star6e_qp_bounds_apply(int rate_mode, MI_VENC_RcParam_t *param, const Star6eQpBounds *b);

#endif
