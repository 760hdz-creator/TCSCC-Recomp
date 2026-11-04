# HUD_Saving use-after-free at mission start (verified 2026-11-03)

Symptom: after the 01_Market load, fatal in `sub_83427D78` (SetVariable "_root.SavingIcon...") through a freed vtable.

## Chain (one thread, deterministic)
1. Loading screen `sub_82D02858` acquires its movies by name (`sub_82D12230`), among them `Ingame\HUD_Saving.swf` (stored at +22436, `sub_82DA8EB0(m,0)`). Only Dynamic_LoadingTimeCodeSubTitle gets the keep bit (`sub_82DA8F98`: movie+120 = 1).
2. Mission HUD build `sub_82E90350` makes the save-icon wrapper (new 52, ctor `sub_83427CB8`, vtable 0x82403000) at mgr+92 (mgr 0x427CC500). Its load `sub_83427FD8` first does a registry lookup `sub_82D10420(sub_82D11588(), name, 0)`: raw pointer from list +76, no refcount. It finds the loading screen's movie and borrows it.
3. Loading-screen teardown `sub_82D02350` -> `sub_82D11278(reg, keep)` releases every name in reg+272 unless keep && movie+120. HUD_Saving has +120 = 0, so `sub_82D0F9F0` queues it and `sub_82D11158` runs the dtor `sub_82DAA4F0`.
4. Event 9 (`sub_82E8F920`, save icon) -> `sub_83427E90` -> `sub_83427D78` -> wrapper+4 vtable+132 = garbage.

No refcount or keep path protects the borrowed pointer, so the reason real hardware does not crash is unknown (maybe a different teardown/HUD order).

## Fix (gfx_log.cpp)
The `sub_83427FD8` hook records wrappers. The dtor hook clears +4 of every live wrapper (vtable still 0x82403000) that points to the dying movie. `sub_83427D78` null-checks +4, so the save icon is skipped. Log: `[gfx] HUD_Saving wrapper ... pointer cleared`.
2026-11-04: `sub_82D0F9F0` (release by name) hook skips the release at the teardown loop call site (lr 0x82D112E4) while a live wrapper holds the movie, so the wrapper keeps a valid movie (the movie+120 keep flag does not help: this teardown passes keep = 0). Skipping other callers hangs (they retry until the movie is gone). The dtor hook stays as a safety net. `SCDA_NO_SAVING_KEEP=1` = old behaviour.

## Tools
`SCDA_TRACE_GFX=1` logs movie ctor/dtor with the guest call chain, plus the wrapper lookup result and its registry list.
