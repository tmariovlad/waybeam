#ifndef PIPELINE_COMMON_H
#define PIPELINE_COMMON_H

#include "sensor_select.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/** Convert GOP duration in seconds to frame count. */
uint32_t pipeline_common_gop_frames(double gop_seconds, uint32_t fps);

/** Build sensor selection config from requested video parameters. */
SensorSelectConfig pipeline_common_build_sensor_select_config(int forced_pad,
	int forced_mode, uint32_t target_width, uint32_t target_height,
	uint32_t target_fps);
/** Log the selected sensor mode and actual frame rate. */
void pipeline_common_report_selected_fps(const char *prefix,
	uint32_t requested_fps, const SensorSelectResult *sensor);
/** Clamp requested image dimensions to maximum supported size. */
void pipeline_common_clamp_image_size(const char *prefix, uint32_t max_width,
	uint32_t max_height, uint32_t *image_width, uint32_t *image_height);

/** Precrop rectangle for aspect ratio correction. */
typedef struct {
	uint16_t x, y, w, h;
} PipelinePrecropRect;

/** Resolve the effective AE max-shutter cap (microseconds) with the canonical
 * precedence: env WAYBEAM_MAX_SHUTTER_US > cfg_max_shutter_us > 1000000/fps.
 * Returns 0 only when all three are 0 (no fps, no cfg, no env) — callers treat
 * 0 as "auto / leave the bin default".  Single source of truth so the one-shot
 * pipeline cap AND the cus3a supervisory thread agree (HB-45: the env used to
 * be honored only at init, then cus3a clobbered it back to the frame period). */
uint32_t pipeline_common_resolve_max_shutter_us(uint32_t cfg_max_shutter_us,
	uint32_t fps);

/** Cap AE max shutter time for target FPS. fps = 0 is a no-op.
 * cfg_max_shutter_us (0 = use frame period) lowers the cap below the AE's
 * bright-light shutter for shorter exposure latency (O94). Precedence:
 * env WAYBEAM_MAX_SHUTTER_US > cfg_max_shutter_us > 1000000/fps. */
int pipeline_common_cap_exposure_for_fps(uint32_t fps, uint32_t cfg_max_shutter_us);

/** Compute frame-lost overshoot threshold in bits/s from target bitrate.
 * Formula: bits + max(20%% margin, 512 kbit/s).  Low bitrates need the
 * absolute floor so I-frames in VBR/AVBR don't trip frame drops.
 * Clamps kbps to 200000 to keep kbps*1024 inside uint32_t. */
uint32_t pipeline_common_frame_lost_threshold(uint32_t kbps);

/** Compute crop rectangle for the VIF/SCL stage.
 * keep_aspect = true  → center-crop the sensor to the aspect ratio of
 *                       image_w x image_h with 2-pixel alignment.
 * keep_aspect = false → return the full sensor area; image_w/image_h are
 *                       ignored and the downstream scaler will stretch. */
PipelinePrecropRect pipeline_common_compute_precrop(uint32_t sensor_w,
	uint32_t sensor_h, uint32_t image_w, uint32_t image_h,
	bool keep_aspect);

/** Resolve which ISP bin path to load.  Resolution order:
 *   1. configured_path is non-empty AND access(R_OK) succeeds → use it
 *   2. else /etc/sensors/<lowercase prefix of sensor_name>.bin if readable
 *      (prefix = sensor_name truncated at first non-alnum, e.g.
 *      "IMX335_MIPI" → "imx335")
 *   3. else write "" (caller skips the load)
 *
 * Logs the chosen path and the reason on stdout (one line, "> ISP bin: ...").
 *
 * out_path must hold at least 256 bytes.  sensor_name may be NULL or
 * empty — the fallback is then skipped.
 *
 * Returns 1 if a path was chosen (out_path filled), 0 if no path
 * could be resolved (out_path = ""). */
int pipeline_common_resolve_isp_bin(const char *configured_path,
	const char *sensor_name, char *out_path, size_t out_sz);

/** Scale QP proportionally for a given band level.
 * level 1 = outermost (weakest), level steps = innermost (full qp).
 * Higher-index ROI regions override lower ones in overlap zones. */
static inline int pipeline_common_scale_roi_qp(int qp, int level, int steps)
{
	int mag;

	if (steps < 1 || level < 1 || level > steps)
		return 0;
	mag = abs(qp);
	mag = (mag * level + steps / 2) / steps;
	return qp < 0 ? -mag : mag;
}

/** Maximum number of ROI band regions. */
#define PIPELINE_ROI_MAX_STEPS 4

#endif /* PIPELINE_COMMON_H */
