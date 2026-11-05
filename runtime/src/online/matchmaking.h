// Session discovery behind XSessionSearch / SearchByID (ported from the TTT2 port). A host publishes its
// matchmaking sessions; searchers list them. The backend follows the transport:
//  - steam: one Steam lobby per session with this record as lobby data (steam_net.cpp; record magic
//    "SCL1", System Link lobbies are "SCC1");
//  - loopback (two instances on one PC): %TEMP%\TCSCCRecomp\sessions\<id>.ses, rewritten every 2 s while
//    the host lives (older than 10 s = gone).
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace mm {

struct Property { uint32_t id = 0; std::vector<uint8_t> value; }; // raw value bytes (big-endian)
struct Context { uint32_t id = 0, value = 0; };

struct SessionRecord
{
    uint64_t id = 0;       // XNKID
    uint64_t hostPeer = 0; // transport peer id of the host
    uint64_t hostXuid = 0; // the host's online XUID
    uint32_t flags = 0;
    uint32_t maxPublic = 0, maxPrivate = 0, filledPublic = 0, filledPrivate = 0;
    uint8_t info[0x3C] = {}; // XSESSION_INFO as the host wrote it
    std::vector<Context> contexts;
    std::vector<Property> properties;
};

void Publish(const SessionRecord& record); // add or update (host)
void Remove(uint64_t id);
// Sessions of other hosts, newest first, at most `max`.
std::vector<SessionRecord> List(uint32_t max);
bool Find(uint64_t id, SessionRecord& out); // also sessions seen in a list or an invite
// A friend's Steam "Join Game" of a Player Match session: the session to join, once.
bool TakeInvite(SessionRecord& out);
void QueueInvite(const SessionRecord& record);
// A Steam lobby record of this kind ("SCL1").
bool IsRecord(const std::vector<uint8_t>& bytes);
bool Parse(const std::vector<uint8_t>& bytes, SessionRecord& out);

} // namespace mm
