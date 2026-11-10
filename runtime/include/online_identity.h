// The profile's online identity (online/xnet.cpp sets it when online play starts; input.cpp's XamUser*
// calls read it). Default: a local profile "Player", not signed in to LIVE.
// Online (RECOMP SETTINGS > ONLINE): signed in to LIVE, the gamertag = the Steam persona name (printable
// ASCII, at most 15 characters; loopback: "Player <n>"), the online XUID made from the transport peer id.
#pragma once
#include <cstdint>
#include <string>

namespace online {
struct Identity
{
    bool live = false;
    std::string gamertag = "Player";
    uint64_t onlineXuid = 0;
};
Identity GetIdentity();
void SetIdentity(const Identity& id);
// online/xsocket.cpp notes each packet from a remote peer; true when one arrived in the last `ms`
// (a System Link / LIVE lobby or match is up).
void NoteRemotePacket();
bool RemotePacketWithin(uint64_t ms);
} // namespace online
