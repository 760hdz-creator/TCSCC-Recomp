# Draw distance / LOD (Conviction)

Found 2026-11-04. Hooks in `runtime/src/lod.cpp`, setting `[Display] DrawDistance`.

## l3d render settings struct

`sub_82C95F30()` returns it (0x402945D0 on 01_Market). The l3d console handler `sub_82C98B00`
parses the developer commands from the strings at 0x821AD700+ and writes these fields:

| offset | type | command | default |
|---|---|---|---|
| +46 | byte | `l3d-distance-fade` (toggle) | 1 |
| +60 | float | small-object cull size (no command found) | 0.0025 |
| +64 | float | cull size override (used when nonzero) | 0 |
| +279 | byte | `l3d-toggle-lod` | 1 |
| +280 | float | `l3d-lod-size2=` | 0.18 |
| +284 | float | `l3d-lod-size1=` | 0.045 |

## Consumers

- LOD pick `sub_82EF31F8(ctx, object)`: projected size (sub_829023D8 on the bounding sphere, distance
  from the view at ctx+23832 +300..308) against r^2 * size2 gives LOD 1 and against r^2 * size1 gives LOD 2.
  The result is clamped to the mesh's LOD count (+88) and stored in object +36 bits 21-22. When +279 is
  0 every object stays at LOD 0. Found with a hardware RW watchpoint (`SCDA_WATCH_LOD=2 SCDA_WATCH_RW=1`).
- Per view, `sub_82F01E28` copies +60 (or +64 when nonzero) to render ctx +23988.
- Cull `sub_82EF2FE8(ctx, object)` returns 1 (not drawn) when the projected size is below
  r^2 * ctx+23988.
- Fade `sub_82EF30A8`: with +46 on, alpha ramps from 1 at 1.5 x cull size down to 0 at the cull size
  (`sub_82EF8800`, which also folds in the per-object bytes +236/+237). With +46 off, objects pop
  instead of fading, so the setting leaves it alone.

Projected size goes as 1/distance^2, so scaling a threshold by k moves its switch distance by
1/sqrt(k). High (k = 0.25) doubles the distance and Ultra (k = 1/16) quadruples it. Maximum turns
off the small-object cull (k = 0) and the LOD switch (+279 = 0).

## A/B (01_Market walk, 30 fps, `SCDA_TRACE_LOD=1` counters, frames 0-6000)

| run | cull tests culled | LOD picks 0/1/2 |
|---|---|---|
| console | 47.2% | 7458 / 1743 / 4993 |
| `SCDA_LOD_OFF=1` | 47.2% | 14535 / 0 / 0 |
| `SCDA_LOD_SCALE=0.25` | 47.3% | 9284 / 2568 / 2448 |
| `SCDA_CULL_SCALE=0.25` | 9.5% | ~unchanged |
| `SCDA_CULL_SCALE=0` | 0% | ~unchanged |

Render time at 30 fps for cull 0 was about +1-1.7 ms per frame. Frame-pair pixel diffs proved
useless here, because the walk script's camera moves between any two frames (even the control run
changes about 274k px in 4 frames). The counters are the evidence.

## Switches

`SCDA_LOD_SCALE=<k>`, `SCDA_CULL_SCALE=<k>` and `SCDA_LOD_OFF=1` override the setting.
`SCDA_LOD_FROM=<frame>` applies those overrides only from that frame on. `SCDA_NO_DISTANCE_FADE=1`
sets +46 to 0. `SCDA_TRACE_LOD=1` logs the struct and the counters, and `SCDA_WATCH_LOD=1|2` arms
watchpoints. Every pass re-reads the game's value: a value that differs from the one last written
becomes the new original, so level loads and console commands still count and Console restores it.
