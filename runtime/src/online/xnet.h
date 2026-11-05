// Online layer shared state (online/xnet.cpp): whether online play is on, and the virtual IN_ADDRs the
// guest uses for peers (ported from the TTT2 port's xlive.cpp). Each remote peer id gets its own
// 10.78.x.y; our own address is 10.77.x.y (also reachable as 127.0.0.1). Our XNADDR carries our peer id
// at +0x18 (abOnline), so a remote's XNADDR (in the game's own packets) resolves to its peer.
#pragma once
#include <cstdint>
#include <vector>
#include <windows.h>

namespace xnet {

// Online play is on (SCDA_ONLINE=1): the network calls work over the transport (net_transport.h);
// off, they report no network as before.
bool Enabled();
// Starts the transport on first use; false when it cannot start.
bool EnsureTransport();
void OnlineStartEarly();
uint64_t LocalPeer();
uint32_t AddrOfPeer(uint64_t peer); // host order
uint64_t PeerOfAddr(uint32_t addr); // 0 when unknown
void Random(void* out, size_t bytes);
// Peers a LAN broadcast reaches: loopback = the other instances on this PC; Steam = peers heard from
// and the session's members (step 3).
std::vector<uint64_t> LanPeers();
void NoteHeardFrom(uint64_t peer);
// The game hosts a System Link match (fscommand CreateSession) / no longer (LeaveMatch, UnInitMPConnection):
// over Steam, a lobby carrying our peer id makes the match findable (searchers' broadcasts reach lobby owners).
void HostStarted();
void HostStopped();
// A LAN broadcast went out (System Link search): over Steam, the lobby list is refreshed in the background
// for a while, and hosts found later get the recent broadcasts (xsocket.cpp XSocketResendBroadcasts).
void NoteBroadcast();
bool Trace(); // SCDA_NET_TRACE=1

} // namespace xnet

// kernel.cpp
uint32_t KernelAddHandle(HANDLE host);
HANDLE KernelHostHandle(uint32_t guest);
bool CloseKernelHandle(uint32_t guest);
