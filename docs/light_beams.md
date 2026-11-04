# Lamp "light wedges" on 01_Market (2026-11-04, open)

Symptom: hard-edged bright white wedges near the street lamps in the cafe opening (user report,
frame ~3600 of the New Game walk, pixels (730,165) and (855,265) at guest 1056x576).

Source: smoke/dust particle sprites. VS 0547ea8658f65ef4 (particle VS, quad list, auto-index, 4 verts
per draw), PS 6c6ab742c8f17444 (alpha blend SRC_ALPHA/INV_SRC_ALPHA) and sibling PS 55d38157138c1c0f
(additive). Atlas 02A39000 (fmt 18 DXT4/5, 1024x1024, 5x5 soft smoke-puff flipbook). RT 7e3 with
color_exp_bias +2 (RB_COLOR_INFO 0x002C0000). ~24 draws per frame.

Measured at the wedge pixel (SCDA_HLSL_OVERRIDE instrumentation + SCDA_PIXEL_WATCH):
- texture sample 0.91, vertex colour ~0.73, vertex alpha 0.60-0.72, near fade 1 (c52 = 0, 1, 0.0099,
  -2.97: invisible < 300 units, full > 400), angle fade block gated by b4 = 0 (off by the title).
- PS b138 (soft-particle depth fade) = 0: off by the title.
- Colour scale = iT[2].x when PS c10.x = 1 (17 of 24 draws) = vertex pos.w (fetch offset 0, float4,
  .wxyz) = 2.89 -> PS colour 1.75, x4 exp bias = 7 HDR, alpha 0.13 x4 = 0.52 -> tone-maps to white.

Ruled out: quad-list expansion (0,1,2)(0,2,3) incl. auto-index; tx_coord_denorm (fetch not denorm);
texture decode/sign; occlusion queries (SCDA_FAKE_OCCLUSION=0: no change); depth test (wall depth
0.0007 behind the sprite, GEQUAL reverse-Z passes); LOG/RCP clamps (FLT_MIN = -FLT_MAX in the prelude);
setp_*_push lanes (fixed in v33 per Xenia, no change here); exp bias on alpha (RGB-only A/B: 4x dimmer,
edges still hard).

Open: everything traced matches the title's data. The only reference found (YouTube "Walkthrough Part
1 - Realistic", 2:43) shows no beams but is the PC version (mouse cursor visible). Need a real 360
(or Xenia) capture of this spot to decide. Unverified suspects: whether Xenos applies color_exp_bias to
alpha (Xenia does), and 7e3 blending precision/clamping vs FP16 host RT.
