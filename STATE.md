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
## Current behaviour
- ~29 presents/s, no fatal in menus. Pre-START screen black (Xenia too). Input script: `SCDA_AUTOPRESS='240:START,400:A:20,500:A:20,600:A:20'` (A needs `:20` hold).
- Scaleform UI works: "Corrupted Data - Player" dialog, autosave notice. GFx OpenFile (sub_83377E98) loads `ingame\3D_MainMenu_1024.swf` etc.
- Main menu = night sky only, lower half black. Frame map (`_run\m900.log`): draws 3-39 Scaleform menu -> RT resolved 0C4D5000 (1024x1024) + 01DE5000, NOTHING samples them; scene = 6 sky meshes (VS 1B66/4617/5120); 18 VisibilityPass boxes (draws 61-78, VS c188ee) all visible; 0 skipped, 0 predicated drops. => CPU never submits world + projected-menu planes.
- More A presses start 01_Market, then crash: sub_83427D78 SetVariable("_root.SavingIcon...") on GFx movie with garbage vtable.
- Bink logos dark red (Xenia: Clancy silhouette white). Upload == guest RAM; plane data dark (Y<=48 @frame 50). Low priority.
- Save shows "corrupted": retest with clean `%LOCALAPPDATA%\TCSCCRecomp` (redirect LOCALAPPDATA).
## Ruled out
- Occlusion, camera, timebase, GPU predication, CP packet drops, texture cache; 26 patch ops + vsel128/eqv/frsqrte/mulhd/vandc/vcmpbfp reviewed correct.
## Debug switches (fresh)
- `SCDA_TRACE_GFX=1` (gfx_log.cpp: OpenFile, movie load, UI fns), `SCDA_TRACE_OCCL=<n>`, `SCDA_TRACE_PRED=1`, `SCDA_SCAN=<val>/<mask>` (watchdog RAM scan), `SCDA_DUMP_TEX_AT=<present>`.
- Existing: `SCDA_TRACE_RESOLVE=<present>` (+`SCDA_TRACE_PS=<hash>`), `SCDA_DUMP_FRAME`+`SCDA_DUMP_BACKBUFFER`, `SCDA_DUMP_DRAWS=<dir>,1`, `SCDA_PROFILE=<start>:<secs>`, `SCDA_TRACE_FS`.
- Clean shader dump: set LOCALAPPDATA to a temp dir + `SCDA_DUMP_HLSL=<dir>`.
## Xenia reference (`_run\xenia`, copy of SCDA build)
- Screenshot: focus window, Alt+D then T -> `screenshots\5553080B\`. Keyboard (`--keyboard_mode=1`): START=X, A=';'. After logos stays black even with START: weak reference.
## Next
1. Find the CPU path that should draw the projected menu planes + world: l3d renderer 0x82ED0000-0x82F10000 (VisibilityPass sub_82EF97D8, occlusion results sub_82EF6278) -> main pass. Menu RT texture header ~0x4014F78C (88000002 AC4D5086 007FE3FF); RT wrappers vt 8206CDA0 at 0x49E97550+ (stride 0x78), no pointer refs (name/CRC binding?).
2. Mission-load crash (HUD_Saving movie object).
3. Corrupted save check; Bink colour; review HEURISTIC jump tables.
4. Gemini bridge returned 503 twice; retry.
