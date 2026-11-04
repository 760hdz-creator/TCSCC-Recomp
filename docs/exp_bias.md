# Colour exponent bias / texture exp_adjust (2026-11-03)

Symptom: 01_Market cafe hard daylight, blown out (reference: night, warm lamp pools).

Cause: three Xenos scale factors the runtime ignored. Conviction's HDR chain relies on all three:
- RB_COLOR_INFO bits 20-25 (signed) `color_exp_bias`: PS output x 2^bias before blending. Scene 7e3 RT
  uses +2 (lighting, draw ~1880 of the walk frame 4500) and +5 (post).
- RB_COPY_DEST_INFO bits 16-21 (signed) `copy_dest_exp_bias`: resolve x 2^bias. 7e3 -> k_16_16_16_16
  (fmt 26, num_format 3 = signed integer) with +10; 7e3 -> k_8_8_8_8 with -4.
- Texture fetch constant dword 3 bits 13-18 (signed) `exp_adjust`: every tfetch x 2^exp_adjust.
  The fmt 26 HDR textures fetch with -15 (num_format integer, signed).
  Net for the scene texture: 2^(2 + 10 - 15) = 1/8.

Implementation (Xenia d3d12_command_processor color_exp_bias, resolve.xesli, XePackFixed):
- `SharedConstants::colorExpBias` (draw_state.cpp), applied in the translated PS main wrapper after
  the alpha test, before Clamp7e3.
- kSaturateCs: flags bits 8-15 = copy bias; clamp by dest number format (unsigned fraction [0,1],
  signed fraction [-1,1], integer = rounded and clamped to the component range, float none).
  Biased resolves always take the CS path.
- `TexExp` in translate.cpp wraps every tfetch macro.
- A/B: `SCDA_NO_EXP_BIAS=1` (all off), `=rt` (RT bias off), `=copy` (resolve bias off). Turning on
  only one of the three makes everything white.

Not the cause: PS 6372c96f (8-tap PCF shadow) writes a shadow-mask channel (RB_COLOR_MASK 1 or 2,
RT at EDRAM base 0), never the scene target; forcing its output to 1 changes nothing visible.
