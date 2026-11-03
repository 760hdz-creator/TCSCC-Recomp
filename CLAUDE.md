# fresh/ — clean Conviction recomp (nested repo inside G:\Project Zero)
All rules in `..\CLAUDE.md` apply (Claude Code loads it too). Differences:
- Own git repo; the outer repo ignores `/fresh/`. Commit here, not in the outer repo.
- `assets/` and `external/` are JUNCTIONS to `..\assets` and `..\external`. Never `rm -rf` them:
  that deletes the shared ISO extract and toolchain. Remove a junction with `cmd /c rmdir assets`.
- Runtime = SCDA runtime + Conviction retarget only (outer commit f243060). Outer-repo runtime
  fixes are NOT ported; port one only when its symptom shows up (list in STATE.md).
