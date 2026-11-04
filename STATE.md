# STATE — fresh/: clean Conviction recomp (Title 5553080B), started 2026-11-03
## Setup
- Nested git repo `G:\Project Zero\fresh` (outer ignores `/fresh/`). `assets/`, `external/` = junctions to outer (never rm -rf; `cmd /c rmdir`).
- XenonRecomp patch `tools/patches/xenonrecomp_pz.patch` = live `external/XenonRecomp` diff vs ddd128b (outer copy STALE).
- Test exe: copy `runtime\build\tcscc.exe` -> `_install\tcscc_fresh.exe`, run from `_install`, log `_install\scda.log`. Shader tests: set LOCALAPPDATA to a temp dir (disk cache keyed by translator version, now v34; any translate.cpp prelude change needs a `kTranslatorVersion` bump).
## Verified
- Image sha 5945a60c... OK. Recomp clean: acceptance counters 0, 99213 functions. HEURISTIC tables reviewed 2026-11-04: labels >= index bound (8255C350 22>=15, 826F1B64 17>=16, 82F72A9C 31>=29), safe.
- Runtime = outer f243060 (SCDA + Conviction retarget); SC4 hooks not built.
## Fixes in fresh (all 2026-11-03 unless noted)
- Ported 04db715, 37a180e (kernel.cpp only), 99a759a. XenonRecomp: jump-table `switch` on rN.u32. XamUserGetSigninInfo/XamUserGetName real.
- Menu world: l3d visibility is GPU PS memexport (PS f374a14f, 1 byte/object, Hi-Z test). Now implemented: MemExport -> u7 window -> guest write-back (6b649b9); param-gen = integer pixel; depth-source resolves = R32 depth + max mip chain (aa212dc); point MINMAX only for VS sizes (166b3a2). GPU culling is the default. `docs/l3d_visibility.md`.
- Mission-load crash: HUD_Saving wrapper borrowed a freed loading-screen movie; 2026-11-04 movie now kept at the teardown (sub_82D0F9F0 hook, lr 82D112E4; `SCDA_NO_SAVING_KEEP=1` = old dtor clear). `docs/hud_saving_uaf.md`.
- Flat-colour world: translator ignored tx_coord_denorm (d176044).
- 2026-11-04 translator v33: setp_*_push p0 from src.w lanes, result from src.x (Xenia), bd504de. v34: vfetch index floored unless is_index_rounded (lamp wedges), 3af885c.
- Speed: texture hash covered tile padding / bytes past packed tails holding live game data (~200 re-conversions/frame); now a level's own elements only (46c98f4). Gameplay at the 30 fps cap, render 7-12 ms/frame.
## Current behaviour
- Menus OK, Scaleform UI OK, title/PRESS START screen renders (2026-11-04).
- 2026-11-04 AUDIO in missions FIXED (1c88745): XAudio XMA kick sub_827F6018 / lock sub_827F5EA8 / array sub_827F5620 hooks (`xma_xaudio.cpp`), MmMapIoSpace -> 0xA0000000|pa. Check: `SCDA_DUMP_AUDIO=<wav>` (6ch float). SCDA_TRACE_XMA stops after 20000 lines.
- 2026-11-04 AUTOPILOT (5189d48): `SCDA_AUTOPILOT=<start>:x,y[,r];...` steers Sam (listener *0x837BEDBC +0x11C; camera = nearest vt 821518E8 actor, int yaw @+0xA4). Actor Location @+0x94, Rotation @+0xA0. Objective managers (vt 8219B270) empty on Market; no goal source yet. Map load by cmdline / file redirect fails (dirty-disc check).
- 2026-11-04 soaks (audio + HUD_Saving + v32 shaders): 01_Market long script to 15000 (8.5 min), 02_Museum/03_Airfield/04_FB-Coste 215-300 s each, no crash.
- 2026-11-03 soak `tools/autopress/01_market_long.txt` (walk + LT cover/RB/Y/RT/B cycles): no crash to frame 30000, cover works; blind input loops between 2 cover spots (no objective steering). Deniable Ops Hunter St. Petersburg Banya (`deniable_banya.txt`: START, DOWN:10, A, START) plays to 9000, 30 fps.
- 01_Market plays from the cafe cutscene to the TAKE COVER tutorial (frame 10000, 5.5 min, no crash, 30 fps): `SCDA_AUTOPRESS=@tools/autopress/01_market_walk.txt` (look pulses from 2800; the prompt appears at 2000-2800).
- Exposure FIXED: colour exp bias (RB_COLOR_INFO +2/+5 on 7e3 HDR RTs, RB_COPY_DEST_INFO +10 into k_16_16_16_16 signed-int resolves) + tfetch exp_adjust (-15) were all ignored. Cafe now night, warm lamp pools (matches reference). `SCDA_NO_EXP_BIAS=1|rt|copy` A/B. `docs/exp_bias.md`.
- GPU light list now DEFAULT (black volumes fixed: signed k_16_16 sphere-occluder falloff read as unorm + depth-only aliased shadow-mask clear colour; also vfetch_mini stride). `SCDA_VISBUF_LIGHTS=off` = old clear. `docs/light_list.md`. Walk to 10000 + menu verified.
- Bink OK: logos are red by design; Market intro cutscene shows natural colours.
- Saves persist. 2026-11-04 NEW-PROFILE "Corrupted Data - Player" FIXED: XamContentGetCreator said IsCreator=1 for missing items, so pre-check sub_824FCE28 went on to load a missing savegame.sav (game raises 0x570 at sub_824FED38). Now missing -> IsCreator 0 + error 3: load skipped, New Game creates savegame.sav/profile.sav, 2nd boot clean. `SCDA_TRACE_SAVE=1` logs result handler sub_824FB378.
- 2026-11-04 SCENE SELECT: `SCDA_UNLOCK_ALL=1` lists all 11 chapters (0a261a1). Menu: START, DOWN (Scene Select), A, DOWN x k, A, A (Rookie), A (overwrite OK). 02_Museum, 03_Airfield, 04_FB-Coste load + render (missions 1-3). Selection is flaky (a rerun of missions 2/3 started 01_Market); missions 4+ always start 01_Market (state @+260 patched to 0; another gate, not found: +0x100 flag? `SCDA_DUMP_MISSIONS=<file>` dumps mission objects). Test with a COPY of LOCALAPPDATA (overwrites the save). Sweep script: scratchpad sweep.ps1.
- 2026-11-04 60 FPS (IN PROGRESS): game clock is delta-time (RAM clock 0x401AAB48: +7.65 s game per 9.1 s wall over 300 frames at 60 fps), so 60 fps is safe for game speed. Caps: menus = presentation interval (`SCDA_VSYNC_HZ=120` lifts, ~46 fps); gameplay = runtime fence round trip. `SCDA_CP_FENCES=2` + `SCDA_FAKE_OCCLUSION=1000` -> 60.0 fps, image OK (debug only: Okhotsk crash / culling risk).
- Fence latency mechanism (frame_queue.h FlushOpenIfWaiting): a mid-frame fence/query the guest waits on is written only after CP idle + 2 ms quiet + renderer idle -> partial-frame handover -> host execute; renderer loop polls with Sleep(1). ~3.7 partials per guest frame (1100 per 10 s). Guest D3D render thread spins in sub_8251F7E8 (fence) / sub_825283A0 (GetData). Timeline captured (SCDA_TIMELINE=150:300 at 120 Hz, 01_Market): scratchpad tl.log, not analysed yet.
- 2026-11-04 Lamp light wedges FIXED (3af885c, v34): vfetch index floored unless is_index_rounded. docs/light_beams.md. Sky vs 360 ref (Mahalo 2:51-2:55) matches (avg RGB within 1-2). v34 sweep: 01_Market, 02_Museum, 03_Airfield, 04_FB-Coste, C01_MafiaClub render OK, no sky flicker (32 consecutive static frames each; Coste only isolated ember/spark pixels).
## Ruled out
- Occlusion queries, camera, timebase, GPU predication, CP packet drops; 26 patch ops + vsel128/eqv/frsqrte/mulhd/vandc/vcmpbfp correct.
- sub_82EFD598 = SONAR pass (gate R+24456 = sonar flag). Light-list black volumes were not PS 6372c96f, Hi-Z or shadow maps (fixed, docs/light_list.md).
## Debug switches
- Visibility: `SCDA_VISBUF_FILL=<hex>|or<hex>|off` (FF = old no-culling), `SCDA_VISBUF_LIGHTS=off`, `SCDA_ALIAS_ZO=1`, `SCDA_NO_EXP_BIAS`, `SCDA_TRACE_BIND=1`, `SCDA_MEMEXPORT_DRAWS=<n>`, `SCDA_NO_MEMEXPORT`, `SCDA_NO_DEPTH_RESOLVE`, `SCDA_DEPTH_FETCH_ONE`, `SCDA_TRACE_HIZ=<n>`, `SCDA_TRACE_VISBUF=<n>`, `SCDA_TRACE_L3D*`, `SCDA_WATCH_L3D=1|2`.
- GPU: `SCDA_TRACE_RESOLVE=<present>` (+`SCDA_TRACE_PS=<hash>`), `SCDA_PIXEL_WATCH=<present>,<x>,<y>`, `SCDA_DUMP_FRAME`+`SCDA_DUMP_BACKBUFFER`, `SCDA_DUMP_DRAWS=<dir>,<step>[,<first>,<last>]`, `SCDA_DUMP_RESOLVES=<dir>`, `SCDA_SKIP_SHADER=<hash>`, `SCDA_TRACE_CONVERT=<present>`, `SCDA_DUMP_HLSL=<dir>`, `SCDA_DUMP_SHADERS=<dir>`.
- CPU: `SCDA_TRACE_GFX=1`, `SCDA_WATCH_WRITE=<hex>[,..][:max]` (+`SCDA_WATCH_RW=1`), `WatchAdd(addr,max)`, `SCDA_PROFILE=<start>:<secs>`, `SCDA_TRACE_FS`, `SCDA_TRACE_INPUT=1`.
- Shader/vertex: `SCDA_HLSL_OVERRIDE=<dir>` (hand-edited <hash>.vs|ps.hlsl), `SCDA_TRACE_VSC=<vs>` (c8-17, 42/43/51/52/95/100), `SCDA_TRACE_PSC=<ps>`, `SCDA_TRACE_VFETCH=<slot>,<dwords>` (vertex records from the draw snapshot).
- Timing/RAM: `SCDA_VSYNC_HZ=<n>`, `SCDA_TRACE_DELAY=1` (sleeps/waits per caller), `SCDA_TIMELINE=<s>:<ms>`, `SCDA_DUMP_RAM=<frames>` (+ tools/ramscan.py, ramdump_peek.py), `SCDA_TRACE_CP=1`, `SCDA_TRACE_SAVE=1`.
- Presented index = frame - 1 (dump of frame 2500 is p2499).
## Next
1. 60 fps runtime fix: cut the fence round trip (analyse tl.log: idle threshold 2 ms -> sub-ms, wake the renderer by event instead of Sleep(1), keep draws-before-fence order for memexport/resolves/queries). Then a 60 fps toggle (presentation interval 1 at real 60 Hz, not the 120 Hz vblank hack), check Bink/Scaleform/cutscene sync, soak maps.
2. Play further: autopilot waypoints are hand-typed (no objective source: AEObjectiveLight vt 82190420 / manager vt 8219B270 arrays empty). Scene Select gate for missions 4+ not found.
3. HUD_Saving icon not yet seen on screen (movie kept, saves OK).
4. Gemini bridge: 2026-11-04 still 503, --cli times out.
5. 8-bit/DXT textures ignore the fetch sign field: none seen yet; renderer logs `[convert] ... sign field ignored` if one shows up.
