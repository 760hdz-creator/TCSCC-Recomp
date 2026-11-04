# Lamp "light wedges" on 01_Market (2026-11-04, FIXED in translator v34)

Symptom: hard-edged bright white wedges near the street lamps in the cafe opening (frame ~3600 of the
New Game walk). Real 360 capture (Mahalo walkthrough, 2:51, same framing) shows none.

Cause: vfetch index conversion. The particle VS 0547ea8658f65ef4 draws one quad per particle record
(quad list, 4 auto-indexed vertices) and fetches its record with index = vertex id * 0.25 (c250.y).
Xenos floors a vfetch index unless the instruction's is_index_rounded bit is set (Xenia
dxbc_shader_translator_fetch). The translator always rounded (uint(index + 0.5)): corners 2 and 3
(0.5, 0.75) read the NEXT particle's record, so every quad stretched between two unrelated particles
(thousands of units apart) -> big hard-edged wedges; their colour scale / alpha came from mixed records.

Fix: FetchVertex receives floor(index) or floor(index + 0.5) per instruction (v34).

How it was found: SCDA_TRACE_VSC=<vs> + SCDA_TRACE_VFETCH=95,44 log the fetch constant and the draw's
snapshot of the vertex records (consecutive records = unrelated particles). Ruled out on the way:
quad expansion, tx_coord_denorm, texture decode, occlusion queries, depth test, near/angle fades,
LOG/RCP clamps, setp_*_push lanes (v33), exp bias on alpha.
