// Steam backend of the online layer (ported from the TTT2 port): App ID 480 (Spacewar), peer packets over
// SteamNetworkingMessages (Valve relays, no port forwarding), sessions as Steam lobbies, a friend's
// "Join Game" as the game's own invite. Built only when the Steamworks SDK is present (SCDA_HAVE_STEAM);
// otherwise every call reports "not running" and the loopback backend is used. steam_api64.dll is
// delay-loaded: a missing DLL or a closed Steam client just means no Steam.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace steamnet {

using Receiver = void (*)(uint64_t fromPeer, bool reliable, const uint8_t* data, size_t size);

// A Steam friend who is online (Friends), playing this game (`inGame`: the same App ID) or not.
struct FriendInfo { uint64_t id = 0; std::string name; bool inGame = false; };

// Starts Steam once (idempotent): false when Steam is unavailable. `receiver` gets every packet on the
// service thread.
bool Start(Receiver receiver);
bool Running();
uint64_t LocalId(); // our SteamID (0 when not running)
std::string LocalName(); // our Steam persona name ("" when not running)
bool Send(uint64_t peer, bool reliable, const void* data, size_t size);

// Lobbies: one per hosted session, carrying `record` (opaque bytes; starts with "SCC1" + u64 session id,
// little-endian) as lobby data, findable by searchers of the same protocol (net::GameProtocol).
void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record);
void RemoveLobby(uint64_t sessionId);
std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs);
// A friend's "Join Game" (GameLobbyJoinRequested_t): the lobby's record, once.
bool TakeJoinRequest(std::vector<uint8_t>& record);
// The player's online friends (those in this game first, then by name; at most 200), and an invite of
// one of them into the Steam lobby of session `sessionId` (hosted here, or found in a lobby search / a
// Join Game) - false when Steam does not run or the lobby is not known.
std::vector<FriendInfo> Friends(int timeoutMs);
bool InviteToSession(uint64_t sessionId, uint64_t friendId, int timeoutMs);

// The calls above run Steam in a HELPER PROCESS (steam_bridge.cpp): this exe started again as
// `--steam-bridge <in> <out>`, talking over two pipes. Steam Input hides every controller from the
// process that runs SteamAPI as app 480 (measured on the TTT2 port, 2026-10-12), so the game process
// never loads Steam. SCDA_STEAM_INPROC=1 = Steam in the game process (diagnostics only: no controller).
int RunBridge(int argc, char** argv); // main() of the helper process

// The in-process implementation (steam_net.cpp), used by the helper.
namespace direct {
bool Start(Receiver receiver);
bool Running();
uint64_t LocalId();
std::string LocalName();
bool Send(uint64_t peer, bool reliable, const void* data, size_t size);
void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record);
void RemoveLobby(uint64_t sessionId);
std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs);
bool TakeJoinRequest(std::vector<uint8_t>& record);
std::vector<FriendInfo> Friends();
bool InviteToSession(uint64_t sessionId, uint64_t friendId);
} // namespace direct

} // namespace steamnet
