---
description: Autonomous loop - triage, XenonAnalyse, XenonRecomp, Gemini on errors, patch, repeat
argument-hint: "[max iterations (default 5)]"
---
Max iterations: $ARGUMENTS. Read `STATE.md` first. Stay inside `G:\Project Zero`. Give an ETA per iteration.

Setup (once; skip what exists):
1. Toolchain: VS BuildTools 2022 `vcvars64.bat` (cmake, ninja), LLVM `clang-cl`, git, python. Missing -> record in `STATE.md` and stop; do not install globally.
2. `external/XenonRecomp` (hedge-dev @ ddd128b + `tools/patches/xenonrecomp_pz.patch` (SCDA ops + Conviction ops, full diff vs ddd128b)) built by `tools/build_xenonrecomp.bat` (run from PowerShell: `& "G:\Project Zero\tools\build_xenonrecomp.bat"`).
3. Flat image: `tools/xex_dump/xex_dump.exe assets/default.xex --out-img assets/default.img --out-header assets/default.xexhdr`; sha256 must match `config/expected_image.txt`.

Loop (stop at max iterations, on success, or on a blocker needing the user):
1. `/triage` if `docs/triage_default.txt` is missing or stale.
2. XenonAnalyse (takes the .xex, not the .img): `external/XenonRecomp/build/XenonAnalyse/XenonAnalyse.exe assets/default.xex config/pz_switches.toml`.
3. `config/pz_config.toml`: save/rest helper addresses come from xex_dump's pattern scan; cite evidence in comments.
4. Before a config change: `python tools/gemini_mcp_server.py --cli "Cross-check this XenonRecomp TOML for Splinter Cell Conviction (5553080B)." config/pz_config.toml docs/triage_default.txt`.
5. `sh tools/recomp.sh` (log `docs/recomp_default.log`), then `python -I tools/check_recomp_acceptance.py docs/recomp_default.log`.
6. Fix by class: unrecognized op -> add to the XenonRecomp patch (rebuild, regenerate patch file); switch error -> `tools/fix_switch_bounds.py`; missing function -> `tools/fix_missing_functions.py`; table_calls -> manual jump table in `config/pz_switches_manual.toml` (or teach XenonAnalyse the pattern). Ask Gemini on unclear ops/patterns, verify against disassembly.
7. Log the iteration (1 line: what failed, what changed) in `STATE.md`.

Done when acceptance prints all zeros. End: run `/compact`, then output a 5-line caveman summary.
