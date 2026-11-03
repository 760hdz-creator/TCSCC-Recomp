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
- Boot now: no fatal over 90 s, Bink logos opened, menu.umd/menu.ass loaded. Test exe: `_install	cscc_fresh.exe` (outer tcscc.exe untouched).
## Known outer-repo fixes NOT ported (cherry-pick from outer when the symptom shows)
- 99a759a: mftb scaled to 50 MHz, KeQueryPerformanceFrequency 50000000 (guest_time).
- 8cea794: k_16 / k_16*_EXPAND texture formats. b5c9e94..fd81c4d: renderer/CP debug traces (SCDA_TRACE_*, SCDA_DUMP_RAM).
- Open outer bug: menu shows sky only, no world/UI (occlusion, camera, timebase ruled out). Bink logo red only.
## Next
1. Screen check: what the window shows (outer: Bink red only, menu sky only). Port 99a759a/8cea794 only if needed.
2. Review the 3 HEURISTIC jump tables and the .text tail decode failures.
