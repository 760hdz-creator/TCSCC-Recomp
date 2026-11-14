# Debug switches (environment variables)

- Visibility: `SCDA_VISBUF_FILL=<hex>|or<hex>|off` (FF = old no-culling), `SCDA_VISBUF_LIGHTS=off`, `SCDA_ALIAS_ZO=1`, `SCDA_NO_EXP_BIAS`, `SCDA_TRACE_BIND=1`, `SCDA_MEMEXPORT_DRAWS=<n>`, `SCDA_NO_MEMEXPORT`, `SCDA_NO_DEPTH_RESOLVE`, `SCDA_DEPTH_FETCH_ONE`, `SCDA_MEMEXPORT_ASYNC=0` (blocking memexport write-back; default async = newest finished window, previous frame when the GPU lags), `SCDA_TRACE_HIZ=<n>`, `SCDA_TRACE_VISBUF=<n>`, `SCDA_TRACE_L3D*`, `SCDA_WATCH_L3D=1|2`.
- GPU: `SCDA_TRACE_RESOLVE=<present>` (+`SCDA_TRACE_PS=<hash>`), `SCDA_PIXEL_WATCH=<present>,<x>,<y>`, `SCDA_DUMP_FRAME`+`SCDA_DUMP_BACKBUFFER`, `SCDA_DUMP_DRAWS=<dir>,<step>[,<first>,<last>]`, `SCDA_DUMP_RESOLVES=<dir>`, `SCDA_SKIP_SHADER=<hash>`, `SCDA_TRACE_CONVERT=<present>`, `SCDA_DUMP_HLSL=<dir>`, `SCDA_DUMP_SHADERS=<dir>`.
- CPU: `SCDA_TRACE_GFX=1`, `SCDA_WATCH_WRITE=<hex>[,..][:max]` (+`SCDA_WATCH_RW=1`), `WatchAdd(addr,max)`, `SCDA_PROFILE=<start>:<secs>`, `SCDA_TRACE_FS`, `SCDA_TRACE_INPUT=1`.
- Shader/vertex: `SCDA_HLSL_OVERRIDE=<dir>` (hand-edited <hash>.vs|ps.hlsl), `SCDA_TRACE_VSC=<vs>` (c8-17, 42/43/51/52/95/100), `SCDA_TRACE_PSC=<ps>`, `SCDA_TRACE_VFETCH=<slot>,<dwords>` (vertex records from the draw snapshot).
- Timing/RAM: `SCDA_VSYNC_HZ=<n>`, `SCDA_TRACE_DELAY=1` (sleeps/waits per caller), `SCDA_TIMELINE=<s>:<ms>`, `SCDA_DUMP_RAM=<frames>` (+ tools/ramscan.py, ramdump_peek.py), `SCDA_TRACE_CP=1`, `SCDA_TRACE_SAVE=1`.
- Presented index = frame - 1 (dump of frame 2500 is p2499).
- Online: `SCDA_ONLINE=0|1`, `SCDA_NET=loopback|steam`, `SCDA_NET_TRACE=1|2`, `SCDA_NET_SELFTEST=1`, `SCDA_NET_DELAY/JITTER/LOSS/SEED`, `SCDA_STEAM_SEE_OWN=1`, `SCDA_STEAM_INPROC=1`, `SCDA_STEAM_NETLOG=<1-8>`, `SCDA_STEAM_HELPER_FAKE=1`.
- Menus: `SCDA_TRACE_FLASH=1` (fscommands + Localize), `SCDA_MENU_ANYWHERE=1`, `SCDA_UNLOCK_ALL=1`, `SCDA_AUTOPRESS=<script>|@<file>`, `SCDA_AUTOPRESS_WALL=1`.
