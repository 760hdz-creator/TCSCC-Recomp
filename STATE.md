# STATE — fresh/: clean Conviction recomp (Title 5553080B), started 2026-11-03
## Setup
- Nested git repo `G:\Project Zero\fresh` (outer ignores `/fresh/`). `assets/`, `external/` = junctions to outer (never rm -rf; `cmd /c rmdir`).
- XenonRecomp patch `tools/patches/xenonrecomp_pz.patch` = live `external/XenonRecomp` diff vs ddd128b (outer copy STALE).
- Test exe: copy `runtime\build\tcscc.exe` -> `_install\tcscc_fresh.exe`, run from `_install`, log `_install\scda.log`. Shader tests: set LOCALAPPDATA to a temp dir (disk cache keyed by translator version, now v31; any translate.cpp prelude change needs a `kTranslatorVersion` bump).
## Verified
- Image sha 5945a60c... OK. Recomp clean: acceptance counters 0, 99213 functions. HEURISTIC tables 8255C350 826F1B64 82F72A9C unreviewed.
- Runtime = outer f243060 (SCDA + Conviction retarget); SC4 hooks not built.
## Fixes in fresh (all 2026-11-03 unless noted)
- Ported 04db715, 37a180e (kernel.cpp only), 99a759a. XenonRecomp: jump-table `switch` on rN.u32. XamUserGetSigninInfo/XamUserGetName real.
- Menu world: l3d visibility is GPU PS memexport (PS f374a14f, 1 byte/object, Hi-Z test). Now implemented: MemExport -> u7 window -> guest write-back (6b649b9); param-gen = integer pixel; depth-source resolves = R32 depth + max mip chain (aa212dc); point MINMAX only for VS sizes (166b3a2). GPU culling is the default. `docs/l3d_visibility.md`.
- Mission-load crash: HUD_Saving wrapper borrowed a freed loading-screen movie; dtor hook clears it (save icon skipped). `docs/hud_saving_uaf.md`.
- Flat-colour world: translator ignored tx_coord_denorm (d176044).
- Speed: texture hash covered tile padding / bytes past packed tails holding live game data (~200 re-conversions/frame); now a level's own elements only (46c98f4). Gameplay at the 30 fps cap, render 7-12 ms/frame.
## Current behaviour
- Menus OK, Scaleform UI OK. Pre-START screen black (Xenia too).
- 01_Market plays from the cafe cutscene to the TAKE COVER tutorial (frame 10000, 5.5 min, no crash, 30 fps): `SCDA_AUTOPRESS=@tools/autopress/01_market_walk.txt` (look pulses from 2800; the prompt appears at 2000-2800).
- Exposure FIXED: colour exp bias (RB_COLOR_INFO +2/+5 on 7e3 HDR RTs, RB_COPY_DEST_INFO +10 into k_16_16_16_16 signed-int resolves) + tfetch exp_adjust (-15) were all ignored. Cafe now night, warm lamp pools (matches reference). `SCDA_NO_EXP_BIAS=1|rt|copy` A/B. `docs/exp_bias.md`.
- GPU light list now DEFAULT (black volumes fixed: signed k_16_16 sphere-occluder falloff read as unorm + depth-only aliased shadow-mask clear colour; also vfetch_mini stride). `SCDA_VISBUF_LIGHTS=off` = old clear. `docs/light_list.md`. Walk to 10000 + menu verified.
- Bink logos dark red (low priority). Save shows "corrupted" (retest with clean LOCALAPPDATA).
## Ruled out
- Occlusion queries, camera, timebase, GPU predication, CP packet drops; 26 patch ops + vsel128/eqv/frsqrte/mulhd/vandc/vcmpbfp correct.
- sub_82EFD598 = SONAR pass (gate R+24456 = sonar flag). Light-list darkening is not the shadow maps (SCDA_DEPTH_FETCH_ONE=1 still black).
## Debug switches
- Visibility: `SCDA_VISBUF_FILL=<hex>|or<hex>|off` (FF = old no-culling), `SCDA_VISBUF_LIGHTS=gpu`, `SCDA_MEMEXPORT_DRAWS=<n>`, `SCDA_NO_MEMEXPORT`, `SCDA_NO_DEPTH_RESOLVE`, `SCDA_DEPTH_FETCH_ONE`, `SCDA_TRACE_HIZ=<n>`, `SCDA_TRACE_VISBUF=<n>`, `SCDA_TRACE_L3D*`, `SCDA_WATCH_L3D=1|2`.
- GPU: `SCDA_TRACE_RESOLVE=<present>` (+`SCDA_TRACE_PS=<hash>`), `SCDA_PIXEL_WATCH=<present>,<x>,<y>`, `SCDA_DUMP_FRAME`+`SCDA_DUMP_BACKBUFFER`, `SCDA_DUMP_DRAWS=<dir>,<step>[,<first>,<last>]`, `SCDA_DUMP_RESOLVES=<dir>`, `SCDA_SKIP_SHADER=<hash>`, `SCDA_TRACE_CONVERT=<present>`, `SCDA_DUMP_HLSL=<dir>`, `SCDA_DUMP_SHADERS=<dir>`.
- CPU: `SCDA_TRACE_GFX=1`, `SCDA_WATCH_WRITE=<hex>[,..][:max]` (+`SCDA_WATCH_RW=1`), `WatchAdd(addr,max)`, `SCDA_PROFILE=<start>:<secs>`, `SCDA_TRACE_FS`, `SCDA_TRACE_INPUT=1`.
- Presented index = frame - 1 (dump of frame 2500 is p2499).
## Next
1. Play further: cover, combat, mark & execute; next crash.
2. tx_coord_denorm at internal scale > 1: guest texture size instead of host Dims2D.
3. Save icon: reload the HUD_Saving movie instead of clearing it. Corrupted save; Bink colour; HEURISTIC jump tables.
4. Gemini bridge returned 503 twice; retry.
5. 8-bit textures (ConvertTexture) still ignore fetch sign bits (signed k_8_8_8_8 normal maps?); check if a signed 8888 shows up.
