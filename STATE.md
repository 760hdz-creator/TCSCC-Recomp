# STATE — fresh/: clean Conviction recomp (Title 5553080B), started 2026-11-03
## Setup
- Nested git repo `G:\Project Zero\fresh` (outer ignores `/fresh/`). `assets/`, `external/` = junctions to outer (never rm -rf; `cmd /c rmdir`).
- Generic tools copied from outer (xex_dump rebuilt). Not copied: installer, ramscan/ramdump/rtrace.
- XenonRecomp patch `tools/patches/xenonrecomp_pz.patch` = live `external/XenonRecomp` diff vs ddd128b. Outer repo's copy is STALE (no u32 switch fix).
- Test exe: copy `runtime\build\tcscc.exe` -> `_install\tcscc_fresh.exe`, run from `_install`, log `_install\scda.log`.
## Verified (2026-11-03)
- Image sha 5945a60c... OK; 8 helper addresses re-found. BINK = separate code section 0x8357D600; decode failures 0x8357D2F8-0x8357D45C = tail of .text.
- Recomp clean (3 fix loops): all acceptance counters 0, 99213 functions (outer 99211; old 99929 stale). HEURISTIC tables 8255C350 826F1B64 82F72A9C unreviewed.
- Runtime = outer f243060 (SCDA + Conviction retarget). SC4 hooks not built; fov.cpp inert.
## Fixes in fresh
- Ported: 04db715 (audio cb arg cell, CS spin count), 37a180e (kernel.cpp only: Nt{Suspend,Resume}Thread pseudo handle), 99a759a (50 MHz timebase, no visible change).
- XenonRecomp: jump-table `switch` on rN.u32 (u64 + addi carry hit __builtin_unreachable, AV in sub_82858B60); default = __builtin_trap.
- XamUserGetSigninInfo (0x28: xuid, state @+0xC, name @+0x18) + XamUserGetName ("Player"): were stubs -> crash after START.
- 2026-11-03 flat-colour world fixed (d176044): translator ignored tx_coord_denorm; full-screen post copies (VS a2b4ed14 has no texcoord, PS uses param-gen pixel pos) sampled one clamped texel. Now coord / Dims2D (exact at scale 1 only). Translator v25.
- 2026-11-03 mission-load crash fixed: HUD_Saving wrapper borrowed the loading screen's movie (registry lookup, no refcount), teardown freed it; dtor hook clears the wrapper's +4 (save icon skipped). `docs/hud_saving_uaf.md`.
- 2026-11-03 MAIN MENU RENDERS (3D room, both Sams, menu planes; 197 -> 1850 draws). Cause: world visibility = GPU PS MEMEXPORT (PS f374a14f971cb1bd writes 1 byte/object, Hi-Z test) into a buffer the CPU reads (sub_82EF9990); translator has no memexport -> bytes 0 -> only skybox. Workaround `l3d_trace.cpp`: bytes = FF after lock (no occlusion culling; `SCDA_VISBUF_FILL=<hex>|off`). Details `docs/l3d_visibility.md`.
## Current behaviour
- ~29 presents/s, no fatal in menus. Pre-START screen black (Xenia too). Input script: `SCDA_AUTOPRESS='240:START,400:A:20,500:A:20,600:A:20'` (A needs `:20` hold).
- Scaleform UI works: "Corrupted Data - Player" dialog, autosave notice. GFx OpenFile (sub_83377E98) loads `ingame\3D_MainMenu_1024.swf` etc.
- Main menu renders fully with the visibility workaround (frame 900 screenshot checked).
- 01_Market RENDERS (Sam at the cafe table, lit + textured, HUD prompts), ~12k draws, ~50 ms/frame with fill FF. Fill 03 = sky only (bits 5-7 = view mask needed), so FF stays.
- Bink logos dark red (Xenia: Clancy silhouette white). Upload == guest RAM; plane data dark (Y<=48 @frame 50). Low priority.
- Save shows "corrupted": retest with clean `%LOCALAPPDATA%\TCSCCRecomp` (redirect LOCALAPPDATA).
## Ruled out
- Occlusion, camera, timebase, GPU predication, CP packet drops, texture cache; 26 patch ops + vsel128/eqv/frsqrte/mulhd/vandc/vcmpbfp reviewed correct.
- sub_82EFD598 = SONAR pass; gate R+24456 = sonar-goggle flag (not a render-world flag). Forcing it -> white screen.
## Debug switches (fresh)
- `SCDA_TRACE_GFX=1` (gfx_log.cpp: OpenFile, movie load/ctor/dtor + guest chain, HUD_Saving wrapper), `SCDA_TRACE_OCCL=<n>`, `SCDA_TRACE_PRED=1`, `SCDA_SCAN=<val>/<mask>` (watchdog RAM scan), `SCDA_DUMP_TEX_AT=<present>`.
- Existing: `SCDA_TRACE_RESOLVE=<present>` (+`SCDA_TRACE_PS=<hash>`), `SCDA_DUMP_FRAME`+`SCDA_DUMP_BACKBUFFER`, `SCDA_DUMP_DRAWS=<dir>,1`, `SCDA_PROFILE=<start>:<secs>`, `SCDA_TRACE_FS`.
- Clean shader dump: set LOCALAPPDATA to a temp dir + `SCDA_DUMP_HLSL=<dir>`; raw ucode `SCDA_DUMP_SHADERS=<dir>`. Translate/compile failures and MEMEXPORT shaders (`[shader] ... uses MEMEXPORT`) go to scda.log.
- l3d (`l3d_trace.cpp`): `SCDA_TRACE_L3D=<n>`, `SCDA_TRACE_L3D_ITEMS=<n>`, `SCDA_SKIP_L3D_BUCKET=<mask>`, `SCDA_WATCH_L3D=1|2`, `SCDA_TRACE_VISBUF=<n>`. `WatchAdd(addr,max)` (profiler.cpp) arms a hardware write watch at run time.
## Xenia reference (`_run\xenia`, copy of SCDA build)
- Screenshot: focus window, Alt+D then T -> `screenshots\5553080B\`. Keyboard (`--keyboard_mode=1`): START=X, A=';'. After logos stays black even with START: weak reference.
## Next
1. Speed: FF fill ~47 ms/frame. PS memexport (6b649b9) + Hi-Z depth resolves (aa212dc) work; `SCDA_VISBUF_FILL=off` renders the intro street at 12 ms but culls big near objects (ground/table/Sam) and lights darker. Next: VS b3b454 bbox loop (i15, near plane), draw 2-3 overlap. `docs/l3d_visibility.md`.
2. Play past the opening: drive input further into 01_Market, look for the next crash / missing effect.
3. tx_coord_denorm at internal scale > 1: pass guest texture size instead of host Dims2D.
4. Save icon: reload the HUD_Saving movie instead of clearing it. Corrupted save check; Bink colour; review HEURISTIC jump tables.
5. Gemini bridge returned 503 twice; retry.
