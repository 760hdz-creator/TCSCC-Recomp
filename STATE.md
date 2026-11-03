# STATE — fresh/: clean Conviction recomp (Title 5553080B), started 2026-11-03
## Setup
- Nested git repo `G:\Project Zero\fresh` (outer ignores `/fresh/`). `assets/`, `external/` = junctions to outer (never rm -rf).
- Tools copied from outer: xex_dump (rebuilt), recomp.sh, find_jump_tables, fix_switch_bounds, fix_missing_functions, check_recomp_acceptance, gen_import_stubs, list_imports, pe_ppc_inspect, xref_addr, stfs, ffmpeg, gemini bridge, tests. Not copied: installer, ramscan/ramdump/rtrace debug tools.
- XenonRecomp patch `tools/patches/xenonrecomp_pz.patch` == live `external/XenonRecomp` diff vs ddd128b (checked 2026-11-03).
## Verified (2026-11-03)
- xex_dump: image sha 5945a60c... matches config/expected_image.txt; 8 helper addresses re-found (same as outer). Sections in docs/xex_dump_default.txt.
- BINK is a separate code section 0x8357D600 size 0xF920 (flags 2). The "Unable to decode" sites are 0x8357D2F8-0x8357D45C = tail of .text (.text ends 0x8357D568), not BINK.
- Config rebuilt from the [main] header only. XenonAnalyse -> 10 tables; find_jump_tables -> 304 more (extent-sized HEURISTIC: 8255C350 826F1B64 82F72A9C, unreviewed).
- RECOMP CLEAN in 3 fix loops: unrecognized=0 switch_errors=0 error_sites=0 cut=0 table_calls=0, functions=99213 (outer: 99211; outer STATE's 99929 was stale). Diff vs outer = only 2 new split functions sub_832AB184, sub_832AB510; all other code identical.
- Runtime = `git archive f243060 runtime` (outer) = SCDA runtime (identical to C:\SCDA-Recomp except import_vars.inc) + Conviction retarget. Import stubs regenerate unchanged (292 imports, 202 implemented, 98 stubbed).
- Guest-address audit of runtime/src: SC4 hooks not built (menu_hooks, travel/audio/wait_trace, xma_xaudio). fov.cpp built but its caller (travel_trace) is not, so it never runs. Other hits are comments.
## Ported from outer (symptom seen on fresh boot, 2026-11-03)
- 04db715 (runtime part): audio callback arg cell + RtlInitializeCriticalSectionAndSpinCount. Symptom: fatal KeWaitForMultipleObjects lr 0x82811AD0.
- 37a180e (kernel.cpp only, pz_trace.cpp debug hooks left out): NtSuspend/ResumeThread pseudo handle. Symptom: main() returns, exit-time AVs (guest 0x400/0x18).
- 99a759a ported (timebase 50 MHz): no visible change in menu.
- Boot now: no fatal over 90 s; SCDA_TRACE_PRESENT 40 s run = 1176 presents, 159 draws/frame (outer tcscc.exe: 1170, 159 = same), Bink logos opened, menu.umd/menu.ass loaded. Test exe: `_install\tcscc_fresh.exe` (outer tcscc.exe untouched).
## Fresh-only fixes (2026-11-03)
- XenonRecomp: jump-table `switch` on rN.u32 (was u64: addi carry 0x100000000 passed cmplwi, hit __builtin_unreachable -> host AV in sub_82858B60); default case = __builtin_trap. Patch file refreshed (541 lines). NOTE: outer tools/patches copy is now stale.
- XamUserGetSigninInfo (0x28 struct: xuid, state 1 @+0xC, name @+0x18) + XamUserGetName ("Player"): stubs left struct garbage -> crash after START.
- Blind input works: `SCDA_AUTOPRESS=240:START,270:A,...` passes the invisible menu and loads 01_Market.umd (draws 159 -> 197 -> load).
- New crash in mission load: sub_83427D78 SetVariable("_root.SavingIcon.txtSavingNormal.txtField.text") on GFx movie with garbage vtable (0xA5FFCFE0).
- GFx WORKS: OpenFile (sub_83377E98) loads swfs incl. ingameD_MainMenu_1024.swf (hooks in runtime/src/gfx_log.cpp, `SCDA_TRACE_GFX=1`). After START the Scaleform "Corrupted Data - Player" dialog renders (A needs `:20` hold: `240:START,400:A:20`). OK -> main menu = night sky, lower half black.
- Main menu frame (m900): draws 3-39 = Scaleform menu into RT -> resolved 0C4D5000 (1024x1024) + 01DE5000; NOTHING samples them. Scene = 6 sky meshes only (VS 1B66/4617/5120). 0 skipped draws, all 18 occlusion objects visible (`SCDA_TRACE_OCCL=<n>`). => game never submits world + projected-menu geometry (CPU side).
- CP drops no predicated packets (`SCDA_TRACE_PRED=1`: 0). 26 patch ops reviewed: update forms match upstream, bdzf (627 real BO=0001y encodings) correct.
- Xenia: screenshot via Alt+D,T works (screenshots/ in _run/xenia), pre-START black too. Desktop capture returns black. Gemini bridge 503 on first try in fresh/.
- Xenia canary (copied to _run/xenia) sees the same shader set (22 VS/26 PS) in 60 s: likely same stall. Screen capture of its window returns black.
- Bink red is a REAL bug: Xenia shows the Clancy logo silhouette white; ours dark red (BT.601 consts + PS verified -> plane data/upload).
- Visibility: renderer l3d uses cell/portal (VCell/VGraph/VPlane, l3d.CellVisibility/ZoneVisibility); 18 VisibilityPass boxes (draws 61-78, VS c188ee) all visible, still no mesh follows. Render fns cluster 0x82ED0000-0x82F10000.
## Known outer-repo fixes NOT ported (cherry-pick from outer when the symptom shows)
- 8cea794: k_16 / k_16*_EXPAND texture formats. b5c9e94..fd81c4d: renderer/CP debug traces (SCDA_TRACE_*, SCDA_DUMP_RAM).
- Open outer bug: menu shows sky only, no world/UI (occlusion, camera, timebase ruled out). Bink logo red only.
## Next
1. Find the consumer of the menu RT 0C4D5000 (3D_MainMenu_1024 -> projected plane material): why the 18 visible objects never reach the main pass. Xenia keyboard: START=X, A=';' (--keyboard_mode=1); Xenia flow after logos differs (stays black), weak reference.
2. If Xenia shows world/text: audit 26 untested XenonRecomp ops by frequency (update forms, bdzf/bdnzt, addme/subfze, lhau).
2. Review the 3 HEURISTIC jump tables and the .text tail decode failures.
