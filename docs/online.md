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
  `proto=tcscc-recomp-1-<version>` (other builds never match: forge #3) + `rec=<hex>`, Join Game, friends, invites, persona name.
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

## Step 5 - Steam invites for Player Match (WORKS on loopback, 2026-11-05)
- Goal: a friend's Steam "Join Game" (or "Invite to game") on a Player Match host joins its XBOX LIVE lobby,
  as Join Game already does for System Link (SCC1 -> host becomes a LAN peer).
- Runtime: Steam join request with an SCL1 record (xnet.cpp Discovery) -> `AcceptInvite`: mm::QueueInvite +
  TakeInvite (= mm::AcceptedInvite) + XN_LIVE_INVITE_ACCEPTED 0x02000002 param user 0.
- Title chain (verified by trace): notify poll sub_824F7428 -> switch sub_824F7278 (0x02000002 -> event 9) ->
  callback 0x82503158 -> online object *0x836E70E4 (0x40899000, vt 82005150) slot 107 sub_82501F98 posts message
  0x20000 -> sub_824F76D8: stores the user's gamertag in obj+200 (sub_82CE7AF8) + UI event 78 (ignored by the
  frontend). UI event 79 handler (sub_82E7D400) sets the pending flag obj+216 (sub_82CE8560) only when
  sub_82502E38 sees XamUserGetSigninInfo flags bit 0 (XUSER_INFO_FLAG_LIVE_ENABLED). The frontend
  (sub_82E7B118) then calls XInviteGetAcceptedInfo, XSessionSearchByID, creates the local session, joins.
- Two fixes made it work: (1) XamUserGetSigninInfo +8 flags = 1 when LIVE (input.cpp; was 0 = flag never set);
  (2) XInviteGetAcceptedInfo = XMsgInProcessCall(0xFC, 0x58023, handle, params): params = 16-byte entries
  {u32 4, pad, u64 value}, count at +512; [0] = &user, [1] = XINVITE_INFO* (0x54). The old guess wrote 0x54 bytes
  at the wrong pointer and corrupted a critical section (0x836EBF50) -> hang.
- Result: B lands in the host's lobby (shown as PRIVATE MATCH, Player 1 + Player 2, "game will start when both
  players are ready"). Not yet tested: real Steam Join Game between two accounts; starting the match after invite.
- Loopback hosts recycle sessions (create/delete) while matchmaking; the test accepts whatever is newest.
- Test: scratchpad invite_test.ps1 pattern: A = _install	cscc_fresh.exe, script
  `240:START,480:DOWN:3,520:DOWN:3,560:DOWN:3,640:A:4,900:A:4,1150:START:4` (COOP & MULTIPLAYER > XBOX LIVE
  PLAYER MATCH > FIND MATCH > Start); B = a copy in another dir, `240:START` + SCDA_TEST_INVITE=55; both
  SCDA_ONLINE=1 SCDA_NET=loopback, own LOCALAPPDATA copies, image path as argument.

## Not done
- Voice chat (XamVoice* / XHV stubs), XUserReadStats (leaderboards), in-game Steam invites, QoS, lag behaviour of
  LAN-speed-designed matches over the internet.
