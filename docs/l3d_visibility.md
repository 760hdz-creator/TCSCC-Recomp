# l3d renderer visibility (Conviction, verified 2026-11-03)

Symptom: main menu showed only the night sky; world, characters and projected menu planes missing.

## Chain (guest addresses)
- Lighting pass `sub_82F0F078(P)` draws buckets via `sub_82F0F018(P, i, name)`; bucket i = { items @P+16+12i, count @P+20+12i } ("Opaque", "OpaqueEmissive", "OpaqueStaticDecals", "Mirrors", ..., "Transparent").
- Buckets filled per pass by `sub_82F0E258` from the visible list of view R[24092]: `*(R+16) + 12*view` { items @+152, count @+156 } (R = *P = renderer, ctor `sub_82EF3CA8`).
- Visible list filled by `sub_82EF8CA8(R, V)` from candidates { prim, u64, mask @+12 } (16 B, items @V+44, count @V+48), kept when prim byte +236 != 0.
- Candidates come from the visibility driver `sub_82EF9990(V)`:
  - always-visible skybox list via `sub_82EF8B18` (only source that worked);
  - world objects: lock (`sub_82511988`, lr 0x82EF99F0) a GPU-written buffer, byte i at ptr + V[184] + i, V[188] objects (354 in the menu). Bits 0+1 = candidate, 2-3 / 4 extra lists, 5-7 view/light mask.
- The bytes are written by the GPU: VisibilityPass draws (from `sub_82EF7E90`, stream constant built by `sub_82EF5CA8`: eA = 0x40000000 | addr>>2, format k_8) with PS `f374a14f971cb1bd`. The PS does 16 depth taps on a downsampled depth texture (512x256) and MEMEXPORTs the byte (r32 = eA, r33 = eM0). Draws 57-59 in the menu frame.
- The shader translator has no memexport: r32/r33 are plain temps, so the buffer stays 0.

## Workaround (l3d_trace.cpp)
After the lock, every object byte = 0xFF (no occlusion culling). Menu: 197 -> 1850 draws, full scene. `SCDA_VISBUF_FILL=<hex>|off`.
Real fix: PS memexport in the translator (UAV write of eM0 to eA address, format from the stream constant) plus a write-back to guest RAM before the CPU lock.

## PS memexport (6b649b9, gated)
- Stream constant (PS c1, built by sub_82EF5CA8): x = 0x40000000 | phys >> 2, y = 2^23 (index added as float), z = 0x4B020200 (format 2 = k_8 in bits 8-13, bit 17 integer), w = 2^23 + small value (NOT the element count).
- Draws: point lists, 1 px points (PA_SU_POINT_SIZE 0x00080008), positions from memory (VS b3b454, fetch slot 95). PS index = x + 256 y of the param-gen pixel (c0 = (1, 256)): param-gen must be the integer pixel (pixel centres gave +128).
- Menu frame: 3 draws (354 / 144 / 41 points) write 0x000-0x161, 0x061-0x1F1, 0x0F1-0x21A; CPU reads +0 x354.
- Implemented: MemExport (translate.cpp) -> u7 window, written range copied back to guest RAM after the draw. Only with SCDA_VISBUF_FILL=off / or<hex>.
- Result: off = 15/354 menu candidates (bits 0-1 almost never set), sky only. or03 = full scene but far too dark, 15k draws (FF 12k), no faster. So both the occlusion bits and the light-list bits (2-7) from the GPU are wrong. Suspects: Hi-Z depth input of f374 (fetch slot 0: does it hit a resolve this frame?), draws 2-3 overwriting draw 1's bytes (last writer wins), eM conversion.

## Hi-Z input (aa212dc)
- f374 slot 0 = depth resolve (copy_src_select 4) of depth base 1D4 to 00532000, 512x256, fetched as k_24_8 with mips (tfetch2DLod). Draws 25-33 (PS 024c1fe3) build the game's own pyramid (depth resolves to 005B2000 ... 005E1000).
- The renderer copied nothing for depth resolves; the fetch matched the HDR scene resolve (fmt 26) that reuses 00532000 later in the frame. Fixed: R32 depth copy + max mip chain per depth resolve, bound for fmt 22/23 fetches.
- In the menu frame the only write to depth 1D4 before the resolve is the clear quad (draw 23, z always): level 0 is all 1.0 (far), so Hi-Z culls nothing there; culling is VS/PS logic.
- Fill off now: intro street correct at 12 ms/frame (FF 19 ms), mission ~1130 candidates. Still wrong: big near objects (ground, table, Sam) culled at some frames, lighting darker, one overexposed frame. Next suspects: VS b3b454 bounding-box loop (loop constant i15, corners behind the camera), draws 2-3 overwriting draw 1 bytes.

## Dead ends
- `sub_82EFD598` (lists "DrawVisibleOpaque"/"DrawVisibleTransparent") is the SONAR pass. Its gate byte R+24456 = S+432 is the sonar-goggle flag (`sub_82D8BF68`, set by the "Gadget_SonarGoogle" handlers `sub_82BC7000` / cleared `sub_82BC6BF0`). Forcing it gives a white screen.
- Draw 60 (VS e66e371ea23144fd, 512 vertices, mask 0) has no memory alloc: dummy draw, not the writer.
- No CPU writer of the buffer (watched C/A/E aliases), no PM4 packet writer (unhandled only 0x3B, 0x46, 0x48).

## Tools
`SCDA_TRACE_L3D=<n>` (bucket counts), `SCDA_TRACE_L3D_ITEMS=<n>`, `SCDA_SKIP_L3D_BUCKET=<hex mask>`, `SCDA_WATCH_L3D=1|2` (hardware write watch on visible-list / candidate count), `SCDA_TRACE_VISBUF=<n>`, `SCDA_DUMP_SHADERS=<dir>` (raw ucode). Translate failures and MEMEXPORT shaders go to scda.log. `SCDA_TRACE_VISBUF` also logs `visbuf read +off xN: nonzero / candidates`.
