# GPU light list: black light volumes (fixed 2026-11-03)

Symptom: with the GPU's l3d light-list bytes (now default), hard-edged black boxes over the
01_Market street and the menu Sams. The old workaround cleared the light list (those lights unused).

Chain (01_market_walk frame 4500, pixel 250,400):
- Material PS fbec9ea0 multiplies its lighting by slot 14 (00B17000), decoded signed.
- 00B17000 = depth-aware upsample (PS 0cc2e7f4) of a 520x288 k_16_16_16_16 occlusion buffer
  (08800230 / colour 00050000, cleared to (0,0,0,1)).
- That buffer is built by VS 972204a8 occluder volumes, blend rgb = src*dstA + dst*srcA,
  a = src.a*dst.a: PS b64d8b9b (per-object 3D occlusion volumes, k_8_8_8_8 tiled 3D) and
  PS dd131dca (analytic spheres: alpha = 1D falloff texture, slot 3).
- Bug 1 (the black): slot 3 is k_16_16 SIGNED (fetch dword 0 sign bits 01 per component),
  ConvertTextureFloat read it as unorm. Now honours sign / biased / integer num_format.
  `SCDA_SKIP_SHADER=dd131dca4c9b7a05` was the decisive A/B.
- Bug 2: the shadow mask (EDRAM 0, 4x 520x288) is cleared by an aliased depth-only clear
  (RB_MODECONTROL 5, depth base == colour base). Console colour = the D24S8 dword. Our
  AliasedClearColor took depth from the viewport z offset (SC:DA Kinshasa tuning) -> (1,0,0,0);
  the quad's raw z (VTE 0x300, z 1.5e-5 = D24 0xFF) + stencil ref 0xFF -> 0xFFFF = (1,1,0,0),
  same as the clear's vertex colour. G-channel shadow lights were fully shadowed. `SCDA_ALIAS_ZO=1`
  restores the old rule.
- Also fixed on the way: vfetch_mini now inherits stride / source / fetch constant from the
  previous vfetch_full (Xenia); it emitted stride 0 (every vertex read vertex 0).

Not it: PS 6372c96f (8-tap PCF) itself, exponent bias, Hi-Z.
Debug: `SCDA_TRACE_BIND=1` (target keys per draw on SCDA_TRACE_RESOLVE frames), snapshot trace
prints vertex dwords 3-6 (clear colour). Pixel watch on a 4x draw reads host (2x, 2y): misleading
across layouts; SCDA_DUMP_DRAWS + pixel reads of the BMPs are reliable. Draw indices drift per run.

Switch: `SCDA_VISBUF_LIGHTS=off` clears the light list (old default).
