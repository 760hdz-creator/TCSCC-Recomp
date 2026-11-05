// Packet transport between game instances (online play, ported from the TTT2 port's net_transport):
// the virtual Xbox sockets and the session layer send whole packets to a peer id; a backend delivers them.
//  - steam (default when the Steam client runs; steam_net.cpp): SteamNetworkingMessages over Valve's
//    relays as App ID 480 (Spacewar), peer id = SteamID. Steam runs in a helper process (steam_bridge.cpp).
//  - loopback: UDP on 127.0.0.1, one port per instance (28200 + n). Peer id = 0x4C4F4F50'0000xxxx with the
//    port in the low bits. Two instances on one PC (both would have the same SteamID).
// SCDA_NET=loopback | steam forces a backend. Tests: SCDA_NET_DELAY=<ms one way>, SCDA_NET_JITTER=<ms>,
// SCDA_NET_LOSS=<% of unreliable packets>, SCDA_NET_SEED (loopback only).
#pragma once
#include <cstddef>
#include <cstdint>

namespace net {

// Channels: reliable ordered (stream sockets, session control) or unreliable (datagrams).
enum class Channel : uint8_t { Unreliable = 0, Reliable = 1 };

using Receiver = void (*)(uint64_t fromPeer, Channel channel, const uint8_t* data, size_t size);

// Starts the backend once (idempotent); packets arrive on its own thread through `receiver`.
bool Start(Receiver receiver);
uint64_t LocalPeer(); // 0 before Start
bool Send(uint64_t peer, Channel channel, const void* data, size_t size);
const char* BackendName(); // "steam", "loopback", or "none" before Start
bool IsLoopbackPeer(uint64_t peer);
uint64_t LoopbackPeer(int index); // the loopback peer id of instance n (port 28200 + n)

// The game's online protocol: only builds of the same one meet (Steam lobbies).
const char* GameProtocol();

} // namespace net
