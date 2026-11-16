# 01_Market harbour water flat brown (GitLab #1) - investigation, 2026-11-16

Symptom (reporter RX 6600 + ours): harbour water = flat brown, no sky/city reflection. Also a small white
"lamp globe" circle near the rail (probably separate). REPRODUCED here.

## Repeatable view (manual stick scripts diverge run to run; autopilot is repeatable)
Scene Select 01_Market, LOCALAPPDATA = copy with a profile, SCDA_UNLOCK_ALL=1, settings ini FrameRate=30 Scale=1:
- SCDA_AUTOPRESS = `240:START,480:DOWN:3,560:A:4,660:A:4,840:A:4,1020:A:4,1400:A:20,1600:A:20,1800:A:20,2000:A:20`
  + look-up pulses `t:RY=1:60,t+150:RY=-1:40` for t = 3000..4500 step 300, `4700:B:6,4900:B:6` (invert look: No),
  `5000:RY=1:150,5300:RY=-1:150,5600:RY=1:200`, then `t:RX=-0.5:12` for t = 6500..8150 step 150.
- SCDA_AUTOPILOT=`6000:-4400,1000,100;-4600,700,60` (reached ~6150). Water on the left of the screen from ~7500;
  frame 8000 guest pixel (100,440) = water. Run time ~5 min (watchdog 330).
- SCDA_TRACE_SPEED=1000 logs Sam position + camera yaw.

## Findings
- Pixel watch (frame 8000, 100,440): opaque draw ps aa326e28f0f96cb0 (0.20,0.16,0.17), then the WATER =
  ps 65474598333bb743 (vs 2fdb8883b1bf1762), blend 07060706, RT 10810410/002C0000 -> 0.104,0.080,0.054 (brown);
  ps 6c6ab742c8f17444 blends on top (same colour).
- Water PS (HLSL dumped with an empty shader cache + SCDA_DUMP_HLSL): 2 scrolling normal maps (s0, s1) ->
  reflection vector -> `tfetchCubeLod` s9 (LOD = max(computed, c0.x)), RGBM decode (rgb*a, squared) x c148.x
  x c4 x fresnel (b142: pow(1-N.V, c0.w) lerp c0.z), lerp to fog c35 by r5.z, saturation c147.w.
  So brown = fog colour / cube sample ~0.
- Cube textures ARE bound (CubeView, d3d11_renderer.cpp ~980): SCDA_TRACE_CUBE at the harbour lists DXT5 (fmt 20)
  cubes 64/128/256, tiled, faces made. Faces dumped to SCDA_DUMP_TEXTURES (114 face bmps) - not yet inspected.

## Next
1. Which cube does the water bind at s9 (pixel log prints tf0-3 only: extend to tf9 / log fetch 9 for PS 6547...)?
   Look at its dumped faces: black/garbage = face stride / tiling / DXT5 RGBM alpha wrong.
2. Check CubeView: face stride for packed mips (Xenos cube faces may include the mip tail per face), generated mips
   vs guest mips (LOD is explicit: generated mips of RGBM data average alpha wrongly but should not give black).
3. Check translator cube(): face/coordinate mapping vs Xenia (tfetchCubeLod, cubeMapData).
4. A/B: SCDA_NO_CUBE=1 (old: no cube) - if identical, the cube contributes nothing (stays 0).
