# Online play (System Link + Player Match over Steam)

Setting: RECOMP SETTINGS > ONLINE = `[Online] Enabled=1`, ON BY DEFAULT since 2026-11-05 (read at start;
`SCDA_ONLINE=0|1` overrides; SCDA_AUTOPRESS runs default off). Off = the
offline runtime's "no network" answers, unchanged. Code: `runtime/src/online/` (transport ported from the TTT2
port, `C:\ttt2\port\src\runtime\kernel`), `online_identity.cpp`, `main_menu.cpp` (CreateSession / LeaveMatch hooks),
`input.cpp` (XamUser* identity), `xam.cpp` (XMsg app 0xFB -> XGI handler), `kernel.cpp` (timers).

## Step 1 - transport (done, 7c3085b)
- `net_transport`: loopback UDP 127.0.0.1:28200+n (peer 0x4C4F4F50'0000|port; `SCDA_NET=loopback`; test
  `SCDA_NET_DELAY/JITTER/LOSS/SEED`) or Steam.
- `steam_net`: app 480 (Spacewar), SteamNetworkingMessages over Valve relays (ICE off), lobbies with
  `proto=tcscc-recomp-1` + `rec=<hex>`, Join Game, friends, invites, persona name.
- `steam_bridge`: Steam runs in `tcscc.exe --steam-bridge <in> <out>` (Steam Input would hide the pads in the game
  process), log `steam_helper.log` next to the exe, restarted if lost. `SCDA_STEAM_INPROC=1` diagnostics.
- SDK: `external/steamworks_sdk_165` (git-ignored); `steam_api64.dll` delay-loaded, copied next to the exe; the
  installer embeds it (IDR_STEAM_DLL, outer 52c773a).
- `SCDA_NET_SELFTEST=1`: loopback ping/pong between instances; Steam lobby publish + search + friends.

## Step 2 - virtual sockets + XNet (done, 567c703)
- `xsocket.cpp`: datagrams (UDP/VDP) + streams over the transport; broadcast (255.255.255.255 / x.x.x.255) to every
  LAN peer; overlapped WSARecvFrom / WSASendTo / WSAGetOverlappedResult. XDK wrapper layout (sub_827EF9B8):
  r3 caller, r4 s, r5 WSABUF*, r6 count, r7 bytes*, r8 flags*(Send: value), r9 from/to, r10 fromlen*/tolen,
  [r1+84] overlapped, [r1+92] completion. Xenia's WSARecvFrom omits fromlen (wrong).
- `xnet.cpp`: XNADDR with our peer id at +0x18, 10.78.x.y per remote peer, 10.77.x.y / 127.0.0.1 ourselves; link
  0x0B; XNetGetTitleXnAddr 0x66; keys; WSA events = host events (handle in r3, no caller).
- Conviction = Quazal: UDP 46000 (LAN matchmaking "AG_LANMM" broadcast), 26000, 9100; VDP 1000 in game.
- Verified (two loopback instances): System Link create -> list -> join -> ready -> co-op C01_MafiaClub on both.

## Step 3 - System Link over Steam (done, 02e4a95)
- Host: fscommand CreateSession -> Steam lobby `SCC1`+peer id; LeaveMatch / UnInitMPConnection -> removed.
- Search: broadcasts reach lobby owners (background ListLobbies every 3 s for 20 s after a search; hosts found late
  get the last 6 s of broadcasts). Steam Join Game -> host is a LAN peer for 10 min.
- Verified with one account (`SCDA_STEAM_SEE_OWN=1`): published + found. Two Steam accounts: NOT yet tested.
- PvP: FACE-OFF works unchanged (lobby: A on MATCH SETTINGS, A on Game Mode -> CO-OP STORY / HUNTER / LAST STAND /
  FACE-OFF). Verified loopback: Face-Off St. Petersburg Banya on both, timers in step 3+ min.

## Step 4 - Xbox LIVE PLAYER MATCH (works on loopback, 2026-11-05)
- Without LIVE sign-in: "You need to be signed into Xbox LIVE". Online on -> signed in to LIVE (state 2), privileges
  granted, gamertag = Steam persona (printable ASCII, 15 max; loopback "Player <n>"), online XUID
  0x0009'0000'0000'0000 | peer & 0xFFFFFFFFFFFF (`online_identity.h`, set in xnet EnsureTransport; transport now
  starts in main() before the game). User asked for the Steam name + avatar: name done; Conviction imports no
  gamer-picture / tile API (lobbies show faction icons) -> avatar has nowhere to show.
- Probe (faked LIVE): COOP & MULTIPLAYER > XBOX LIVE PLAYER MATCH opens FIND MATCH (Start = matchmaking, Y = Private
  Match). Matchmaking = pure XSession, no Ubisoft servers: XMsg app 0xFB 0xB0006 (XUserSetContext), 0xB0007
  (XUserSetProperty), 0xB0016 (XSessionSearch) -> nothing found -> XamSessionCreateHandle, 0xB0010 (XSessionCreate),
  XNetCreateKey, VDP socket.
- Written (builds, untested): `online/xsession.cpp` (TTT2 port: contexts/properties, Create/Delete/Join/Leave/
  Start/End/Modify/Search(GAME_TYPE 0x800A / GAME_MODE 0x800B match)/SearchByID/ArbitrationRegister,
  XamSessionCreateHandle/RefObjByHandle), `online/matchmaking.cpp` (records "SCL1": Steam lobbies, loopback
  %TEMP%\TCSCCRecomp\sessions\<id>.ses heartbeat 2 s / stale 10 s), xam.cpp XMsgStartIORequest app 0xFB ->
  registered XGI handler (searches async on a copy of the block; failures = ERROR_FUNCTION_FAILED + HRESULT at
  XOVERLAPPED+0x18).
- Verified 2026-11-05 (two loopback instances): A Start -> 0 sessions -> creates + publishes; B Start -> finds A,
  XSessionJoinRemote, both in the XBOX LIVE lobby (Archer + Kestrel); X on both -> READY -> LOADING MATCH IN ->
  XSessionStart -> co-op St. Petersburg Banya loads and plays on both. No unhandled XGI messages logged.
- Still open: Steam Join Game / invites for SCL1 records (mm::QueueInvite + XN_LIVE_INVITE_ACCEPTED /
  XInviteGetAcceptedInfo), Private Match invites, XNetQosLookup (not called so far), two Steam accounts.
- Test runs: `SCDA_ONLINE=1 SCDA_NET=loopback`, second instance from scratchpad `inst2` with its own LOCALAPPDATA
  copy (`lad2`) and `"G:\Project Zero\_install\game\default.img"` as argument. Each PowerShell call starts with a
  clean environment (set every variable in the same call).

## Not done
- Voice chat (XamVoice* / XHV stubs), XUserReadStats (leaderboards), in-game Steam invites, QoS, lag behaviour of
  LAN-speed-designed matches over the internet.
