# H.264 Codec Support & Resilience Parity

**Priority: MEDIUM** | **Status: Implemented end-to-end (cross-compile verified star6e+maruko, zero warnings; on-device behaviour HIL-unverified)**

> Implemented 2026-06-23. `video0.codec` selects `h265` (default) or `h264`; the
> selection flows through the entire pipeline — encoder, RTP packetization, RTP
> payload type, TS recording, and **all four FPV resilience levers**. Built clean
> on both backends; every SDK-dependent change is compile-verified against the
> real SigmaStar SDK. Glass-to-glass latency impact remains HIL (not measured).

---

## 1. Summary

H.264 was retired in 0.10.12 (the encoder kept its H.264 paths, but the codec
was hardwired to H.265 across the RTP/config/recording chain). It is now wired
back **end-to-end** and brought to **full resilience parity** with H.265.

| Layer | H.264 status | Key files |
|-------|--------------|-----------|
| Encoder (VENC) | already supported (RC paths existed) | `star6e_pipeline.c`, `maruko_pipeline.c` |
| Config selection | `video0.codec` (h265 default / h264) | `venc_config.{c,h}`, `codec_config.{c,h}` |
| HTTP API | settable + validated (restart-required) | `venc_api.c` |
| RTP packetization | FU-A type 28 (vs HEVC 49) | `rtp_packetizer.c`, `hevc_rtp.c` |
| RTP payload type | 96 (vs HEVC 97) | `rtp_session.c` |
| Per-frame send | codec-aware NAL loop | `star6e_video.c` / `star6e_hevc_rtp.c`, `maruko_video.c` |
| TS recording | PMT stream-type 0x1B (vs 0x24) | `ts_mux.{c,h}`, `*_ts_recorder.c` |
| Intra-refresh | codec-agnostic (works) | `*_pipeline.c` |
| SVC-T refPred | codec-agnostic (works) | `star6e_pipeline.c` |
| Non-reference marking | H.264 `nal_ref_idc`→0 | `star6e_runtime.c`, `maruko_pipeline.c` |
| Multi-slice split | `MI_VENC_SetH264SliceSplit` | `star6e_mi.c`, `maruko_mi.c`, `*_pipeline.c` |
| Super-frame cap | `MI_VENC_SetSuperFrameCfg` | `star6e_mi.c`, `maruko_mi.c`, `*_pipeline.c` |

**Net:** on the SSC338Q (star6e) and the HiSilicon (maruko) backends, H.264 has
no functional or resilience disadvantage versus H.265.

---

## 2. Config

```json
"video0": {
  "codec": "h264",        // "h265" (default) or "h264"
  "rcMode": "cbr",        // cbr / vbr / avbr / qvbr (RC-mode enum is codec-dependent)
  "sliceRows": 16,        // multi-slice split (both codecs)
  "superframeKbits": 0,   // super-frame cap, 0 = off (both codecs)
  "resilience": "fpv"     // intra-refresh + SVC-T refPred (both codecs)
}
```

- `codec` is **restart-required** (changing it rebuilds the encode chain).
- Settable over HTTP: `GET /api/v1/set?video0.codec=h264`. Unknown values return
  a validation error (`codec must be 'h265' or 'h264'`).
- The RC-mode enum differs by codec and is resolved in
  `codec_config_resolve_codec_rc()`: CBR=3 (both); VBR H265=4/H264=2;
  AVBR H265=5/H264=0; QVBR H265=6/H264=1. Verified against `fill_maruko_rc_attr()`
  and `star6e_pipeline_start_venc()`.

---

## 3. RTP packetization (RFC 6184 / RFC 7798)

The packetizer is dual-codec. NALs are never aggregated (single-NAL + FU-A only),
so the wire stays compatible with minimal receivers.

| | H.264 (RFC 6184) | HEVC (RFC 7798) |
|--|------------------|-----------------|
| NAL header | 1 byte | 2 bytes |
| FU type | 28 | 49 |
| FU header | S/E/R + type(5) | S/E + type(6) |
| IDR NAL type | 5 | 19 / 20 |
| Param sets | SPS(7)/PPS(8) | VPS(32)/SPS(33)/PPS(34) |
| RTP payload type | 96 | 97 |

`hevc_rtp_*` are kept as thin `PT_H265` wrappers over the codec-aware
`h26x_rtp_send_nal()` / `h26x_rtp_prepend_param_sets()`, so existing HEVC
callers are unchanged (zero regression).

---

## 4. Resilience levers — full parity (all 4)

All four FPV resilience levers work on both codecs. Two were already
codec-agnostic; two needed wiring.

1. **Intra-refresh (rolling GDR)** — `MI_VENC_SetIntraRefresh{bEnable,lines,qp}`
   has no codec field; already worked on H.264. (A misleading
   `(void)codec; /* H.265 only */` comment was corrected — it never gated.)

2. **SVC-T refPred pyramid** — `MI_VENC_SetRefParam{u32Base,u32Enhance,bEnablePred}`
   is codec-agnostic; `apply_ref_pred` is not codec-gated. Already worked.

3. **Non-reference NAL marking** — when the SDK flags a frame
   `ENHANCE_P_NOTFORREF`, the runtime rewrites each coded slice NAL header so a
   generic decoder can drop it cleanly under loss:
   - HEVC: `TRAIL_R`(0x02) → `TRAIL_N`(0x00)
   - H.264: clear `nal_ref_idc` (mask 0x60) on the coded non-IDR slice (type 1)
     → non-reference picture (`star6e_rewrite_nonref` / `maruko_rewrite_nonref`).
   The refPred call site reads `h264Info.refType` / `h265Info.refType` (both
   expose `refType` in the strminfo union) and runs for both codecs when
   `ref_base > 0`.

4. **Multi-slice split** — `MI_VENC_SetH264SliceSplit` and `…H265SliceSplit`
   both exist in `libmi_venc.so`; they share the
   `{bSplitEnable, u32SliceRowCount}` cfg struct (confirmed via Divinus
   `i6_venc_slicesplit`). The pipeline picks the setter by codec.

(Note: `resilience` presets that set `ref_base`/intra-refresh defaults were
tuned with HEVC in mind, but the underlying SDK calls are codec-agnostic, so
the presets apply on H.264 too.)

---

## 5. Super-frame cap (anti-spike) — `video0.superframeKbits`

Caps per-frame bit bursts so a single oversized frame can't stall the wfb link
(steadier latency). When a coded frame exceeds the threshold the SDK re-encodes
it (`MI_VENC_SUPERFRM_REENCODE`).

- `superframeKbits` = P/B threshold in kbits; the I threshold = 4× (an IDR is
  legitimately larger). 0 = off.
- `MI_VENC_SuperFrameCfg_t` = `{eSuperFrmMode, u32SuperIFrmBitsThr,
  u32SuperPFrmBitsThr, u32SuperBFrmBitsThr}`, `sizeof 16`. Layout confirmed
  against Divinus `i6_venc_superframe` ("field-for-field verified"); a
  `_Static_assert(sizeof == 16)` guards the ABI at compile time.

This closed the last lever asymmetry vs Divinus — see §7.

---

## 6. Verification

- **Cross-compiled both backends** (`make SOC_BUILD=star6e build`,
  `SOC_BUILD=maruko build`) with the vendored ARM toolchain
  (`arm-openipc-linux-gnueabihf-gcc 13.3.0`) — **zero warnings/errors**. Every
  SDK-dependent change compiles against the real SigmaStar SDK; the
  `_Static_assert` on the super-frame struct passing proves the ABI matches.
- `nm` / `strings` confirm the symbols are linked into both binaries:
  `rtp_packetizer_send_h264_fu/_nal`, `codec_config_resolve_codec`,
  `star6e_hevc_rtp_send_frame_codec`, `MI_VENC_SetH264SliceSplit`,
  `MI_VENC_SetSuperFrameCfg`.
- **Host unit tests** (SDK-free TUs, in `repos/sil/pkt/waybeam/`):
  - packetizer H.264 FU-A (5 tests) + 40+ HEVC (no regression)
  - codec resolution + RC-mode mapping + payload type (19 tests)
  - TS-mux H.264 PMT stream-type 0x1B (6 tests)
  - non-reference rewrite bitwise (12 tests)
- Bench on a real ffmpeg H.264 720p stream: 665 NALs → 1695 RTP packets
  (1237 FU-A type 28 + 458 single).

**HIL-unverified:** glass-to-glass latency and on-device behaviour. This work is
correctness/plumbing proven by cross-compile + unit tests, not a latency
measurement.

---

## 7. Streamer parity (waybeam vs Divinus)

A code-vs-code audit (verifying SDK symbols with `strings` on `libmi_venc.so`,
not assuming) found each streamer had a lever the other lacked, with all symbols
present in the SDK:

| Lever | waybeam | Divinus |
|-------|---------|---------|
| Intra-refresh | ✅ | ✅ |
| Slice-split (H264+H265) | ✅ | ✅ |
| Super-frame cap | ✅ (wired 2026-06-23) | ✅ |
| SVC-T refPred | ✅ | ❌ |
| Frame-lost strategy | ✅ | ❌ |

After wiring super-frame cap, **waybeam is a strict superset of Divinus** on
encode/resilience levers (everything Divinus has, plus refPred + frame-lost).
Divinus remains an archive (project decision ADR-021) and is not brought to
parity.

---

## 8. Lessons (HM-011 — probe the system, don't assume)

Three times this session a "feature X is missing / impossible on H.264" claim
turned out to be wrong because the assumption wasn't checked against the actual
`libmi_venc.so` symbols:

1. "intra-refresh / refPred are inert under H.264" — wrong; both SDK calls are
   codec-agnostic and already worked.
2. "slice-split is H.265-only (no H.264 SDK symbol)" — wrong;
   `MI_VENC_SetH264SliceSplit` exists in all three `.so`.
3. (Avoided) — super-frame struct layout was taken from Divinus
   ("field-for-field verified") + guarded with `_Static_assert`, not guessed.

**Rule for future "lever X is missing" claims:** check
`strings libmi_venc.so | grep <Symbol>` first, not just whether waybeam binds it.
