// XGI (XMsg app 0xFB): user contexts / properties, XSession objects (online step 4: Xbox LIVE PLAYER MATCH;
// ported from the TTT2 port's xsession.cpp without its ranked / lounge / stats parts). Host sessions that use
// matchmaking or the peer network are published through matchmaking.h; searches and searches by id read
// them back. Guest structs are big-endian.
#include <windows.h>
#include "xnet.h"
#include "matchmaking.h"
#include "net_transport.h"
#include <online_identity.h>
#include <runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t S_OK_ = 0, E_FAIL_ = 0x80004005u, E_INVALIDARG_ = 0x80070057u;
constexpr uint32_t ERROR_FUNCTION_FAILED_ = 0x65B, STATUS_INVALID_HANDLE_ = 0xC0000008u;
constexpr uint32_t X_ONLINE_E_SESSION_NOT_FOUND = 0x80155200u, X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER = 0x80155207u;

// XSessionCreate flags
constexpr uint32_t kHost = 0x1, kStats = 0x4, kMatchmaking = 0x8, kPeerNetwork = 0x20;

uint64_t Read64(uint32_t a) { return (uint64_t(GuestLoad32(a)) << 32) | GuestLoad32(a + 4); }
void Write64(uint32_t a, uint64_t v)
{
    GuestStore32(a, uint32_t(v >> 32));
    GuestStore32(a + 4, uint32_t(v));
}

// Each distinct message once (SCDA_NET_TRACE=1: all).
void Log(const std::string& key, const char* fmt, ...)
{
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::lock_guard l(m);
        if (!xnet::Trace() && !seen.insert(key).second)
            return;
    }
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    RuntimeLog("[xsession] %s\n", line);
}

// Our own XNADDR (xnet.cpp's layout: the peer id at +0x18).
void WriteOwnXnAddr(uint32_t a)
{
    std::memset(g_base + a, 0, 0x24);
    const uint64_t peer = xnet::LocalPeer();
    const uint32_t ina = 0x0A4D0000u | uint32_t(peer & 0xFFFF);
    GuestStore32(a + 0x00, ina);
    GuestStore32(a + 0x04, ina);
    GuestStore16(a + 0x08, 3074);
    for (int i = 0; i < 6; ++i)
        g_base[a + 0x0A + i] = uint8_t(peer >> (8 * i)) | (i == 0 ? 0x02 : 0);
    Write64(a + 0x18, peer);
    g_base[a + 0x20] = 1;
}

enum class State : uint32_t { Lobby, Registration, InGame, Reporting, Deleted };
struct Member { uint64_t xuid = 0; uint32_t userIndex = 0xFE; bool isPrivate = false; };
struct Session
{
    uint32_t guestObject = 0; // X_KSESSION {be u32 handle}
    bool created = false;
    uint32_t flags = 0, maxPublic = 0, maxPrivate = 0;
    uint64_t nonce = 0, id = 0;
    std::vector<uint8_t> info; // XSESSION_INFO (0x3C)
    std::vector<Member> members;
    State state = State::Lobby;
};

std::mutex g_mutex;
std::map<uint32_t, Session> g_sessions;
uint32_t g_nextSession = 0xD3000000u;
std::map<uint32_t, uint32_t> g_contexts;                  // user 0 context id -> value
std::map<uint32_t, std::vector<uint8_t>> g_properties;    // user 0 property id -> bytes

Session* Lookup(uint32_t objectPtr)
{
    if (!objectPtr)
        return nullptr;
    const auto it = g_sessions.find(GuestLoad32(objectPtr));
    return it == g_sessions.end() ? nullptr : &it->second;
}

// A host's matchmaking / peer-network session, as searchers see it (under g_mutex).
void Publish(const Session& s)
{
    if (!(s.flags & kHost) || !(s.flags & (kMatchmaking | kPeerNetwork)) || s.state == State::Deleted || s.info.size() != 0x3C)
        return;
    mm::SessionRecord r;
    r.id = s.id;
    r.hostPeer = xnet::LocalPeer();
    r.hostXuid = online::GetIdentity().onlineXuid;
    r.flags = s.flags;
    r.maxPublic = s.maxPublic;
    r.maxPrivate = s.maxPrivate;
    for (const Member& m : s.members)
        (m.isPrivate ? r.filledPrivate : r.filledPublic)++;
    std::memcpy(r.info, s.info.data(), sizeof(r.info));
    for (const auto& [id, value] : g_contexts)
        r.contexts.push_back({ id, value });
    for (const auto& [id, value] : g_properties)
        r.properties.push_back({ id, value });
    mm::Publish(r);
}

// 0xB0010 XSessionCreate {obj, flags, maxPublic, maxPrivate, user, pSessionInfo, pNonce}
uint32_t Create(uint32_t b)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(GuestLoad32(b));
    if (!s || s->created)
        return ERROR_FUNCTION_FAILED_;
    const uint32_t flags = GuestLoad32(b + 4), info = GuestLoad32(b + 0x14), nonceOut = GuestLoad32(b + 0x18);
    if (!info)
        return ERROR_FUNCTION_FAILED_;
    s->created = true;
    s->flags = flags;
    s->maxPublic = GuestLoad32(b + 8);
    s->maxPrivate = GuestLoad32(b + 0xC);
    if ((flags & kHost) || flags == kStats || flags == 0)
    {
        xnet::Random(&s->nonce, sizeof(s->nonce));
        if (nonceOut)
            Write64(nonceOut, s->nonce);
        uint64_t id = 0;
        xnet::Random(&id, sizeof(id));
        s->id = (flags & ~kHost) ? (0x8000000000000000ull | (id & 0x00FFFFFFFFFFFFFFull)) : (id & 0x00FFFFFFFFFFFFFFull);
        Write64(info, s->id);           // XNKID sessionID
        WriteOwnXnAddr(info + 0x08);    // XNADDR hostAddress
        xnet::Random(g_base + info + 0x2C, 16); // XNKEY
    }
    else
    {
        s->id = Read64(info); // joining: the search result's id
        if (nonceOut)
            s->nonce = Read64(nonceOut);
    }
    s->info.assign(g_base + info, g_base + info + 0x3C);
    Log("create" + std::to_string(flags), "XSessionCreate(flags %03X, public %u, private %u) -> %016llX", flags, s->maxPublic, s->maxPrivate,
        (unsigned long long)s->id);
    Publish(*s);
    return S_OK_;
}

// 0xB0012 / 0xB0013 Join / Leave {obj, count, xuids (0 = local), userIndices, private}
uint32_t JoinLeave(uint32_t b, bool join)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(GuestLoad32(b));
    if (!s)
        return STATUS_INVALID_HANDLE_;
    const uint32_t count = GuestLoad32(b + 4), xuids = GuestLoad32(b + 8), users = GuestLoad32(b + 0xC), privates = GuestLoad32(b + 0x10);
    for (uint32_t i = 0; i < count && i < 16; ++i)
    {
        Member m;
        if (xuids)
            m.xuid = Read64(xuids + i * 8);
        else
        {
            m.userIndex = users ? GuestLoad32(users + i * 4) : 0;
            m.xuid = online::GetIdentity().onlineXuid + m.userIndex;
        }
        m.isPrivate = privates && GuestLoad32(privates + i * 4);
        auto it = std::find_if(s->members.begin(), s->members.end(), [&](const Member& x) { return x.xuid == m.xuid; });
        if (join && it == s->members.end())
            s->members.push_back(m);
        if (!join && it != s->members.end())
            s->members.erase(it);
    }
    Log(std::string(join ? "join" : "leave") + (xuids ? "remote" : "local"), "XSession%s%s(%u)", join ? "Join" : "Leave", xuids ? "Remote" : "Local", count);
    Publish(*s);
    return S_OK_;
}

uint32_t SetState(uint32_t b, State state, const char* name)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(GuestLoad32(b));
    if (!s)
        return STATUS_INVALID_HANDLE_;
    s->state = state;
    Log(name, "XSession%s(%016llX)", name, (unsigned long long)s->id);
    return S_OK_;
}

// Search results: XSESSION_SEARCHRESULT_HEADER {count, pResults}, the results (0x5C each: XSESSION_INFO,
// open public / private, filled public / private, cProperties, cContexts, pProperties, pContexts), then their
// XUSER_PROPERTY (0x18) / XUSER_CONTEXT (8) arrays and string / binary payloads, inside the caller's buffer.
uint32_t WriteResults(const std::vector<mm::SessionRecord>& found, uint32_t results, uint32_t size)
{
    const uint32_t count = uint32_t(found.size());
    uint32_t tail = results + 8 + count * 0x5C;
    const uint32_t end = results + size;
    GuestStore32(results, count);
    GuestStore32(results + 4, results + 8);
    for (uint32_t i = 0; i < count; ++i)
    {
        const mm::SessionRecord& r = found[i];
        const uint32_t e = results + 8 + i * 0x5C;
        std::memcpy(g_base + e, r.info, 0x3C);
        GuestStore32(e + 0x3C, r.maxPublic > r.filledPublic ? r.maxPublic - r.filledPublic : 0);
        GuestStore32(e + 0x40, r.maxPrivate > r.filledPrivate ? r.maxPrivate - r.filledPrivate : 0);
        GuestStore32(e + 0x44, r.filledPublic);
        GuestStore32(e + 0x48, r.filledPrivate);
        const uint32_t props = tail;
        uint32_t nProps = 0;
        if (props + uint32_t(r.properties.size()) * 0x18 <= end)
        {
            tail += uint32_t(r.properties.size()) * 0x18;
            for (const mm::Property& p : r.properties)
            {
                const uint32_t at = props + nProps * 0x18;
                std::memset(g_base + at, 0, 0x18);
                GuestStore32(at, p.id);
                const uint8_t type = uint8_t(p.id >> 28);
                g_base[at + 8] = type;
                if (type == 4 || type == 6) // UNICODE / BINARY: payload in the tail
                {
                    const uint32_t n = uint32_t(p.value.size());
                    if (tail + n > end)
                        continue;
                    if (n)
                        std::memcpy(g_base + tail, p.value.data(), n);
                    GuestStore32(at + 0x10, n);
                    GuestStore32(at + 0x14, tail);
                    tail += (n + 3) & ~3u;
                }
                else
                    std::memcpy(g_base + at + 0x10, p.value.data(), (std::min<size_t>)(p.value.size(), 8));
                ++nProps;
            }
        }
        const uint32_t ctxs = tail;
        uint32_t nCtx = 0;
        if (ctxs + uint32_t(r.contexts.size()) * 8 <= end)
        {
            for (const mm::Context& c : r.contexts)
            {
                GuestStore32(ctxs + nCtx * 8, c.id);
                GuestStore32(ctxs + nCtx * 8 + 4, c.value);
                ++nCtx;
            }
            tail += nCtx * 8;
        }
        GuestStore32(e + 0x4C, nProps);
        GuestStore32(e + 0x50, nCtx);
        GuestStore32(e + 0x54, nProps ? props : 0);
        GuestStore32(e + 0x58, nCtx ? ctxs : 0);
    }
    return S_OK_;
}

// 0xB0016 / 0xB001C XSessionSearch(Ex) {proc, user, numResults, u16 numProps, u16 numContexts, props, contexts,
// cbResults, pResults[, numUsers]}: published sessions of other hosts with open public slots whose
// GAME_TYPE / GAME_MODE (0x800A / 0x800B) match the searcher's (the title's XLAST rules are not evaluated).
uint32_t Search(uint32_t b)
{
    const uint32_t proc = GuestLoad32(b), numResults = GuestLoad32(b + 8);
    const uint32_t numContexts = GuestLoad16(b + 0xE), contexts = GuestLoad32(b + 0x14);
    const uint32_t size = GuestLoad32(b + 0x18), results = GuestLoad32(b + 0x1C);
    if (!results)
        return E_INVALIDARG_;
    if (size < 8 + 0x5C * numResults)
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
    std::map<uint32_t, uint32_t> wanted;
    std::string text;
    for (uint32_t i = 0; contexts && i < numContexts && i < 32; ++i)
    {
        const uint32_t id = GuestLoad32(contexts + i * 8), value = GuestLoad32(contexts + i * 8 + 4);
        char one[32];
        snprintf(one, sizeof(one), " %08X=%u", id, value);
        text += one;
        if (id == 0x800A || id == 0x800B)
            wanted[id] = value;
    }
    std::vector<mm::SessionRecord> found;
    for (mm::SessionRecord& r : mm::List(64))
    {
        if (found.size() >= numResults)
            break;
        if (r.maxPublic <= r.filledPublic)
            continue;
        bool match = true;
        for (const auto& [id, value] : wanted)
            for (const mm::Context& c : r.contexts)
                if (c.id == id && c.value != value)
                    match = false;
        if (match)
            found.push_back(std::move(r));
    }
    Log("search" + std::to_string(proc) + "_" + std::to_string(found.size()) + text, "XSessionSearch(proc %u, %u results, contexts%s) -> %zu sessions", proc,
        numResults, text.c_str(), found.size());
    return WriteResults(found, results, size);
}

// 0xB001B XSessionSearchByID {user, XNKID (8, at +4), cbResults, pResults}
uint32_t SearchById(uint32_t b)
{
    const uint64_t id = Read64(b + 4);
    const uint32_t size = GuestLoad32(b + 0xC), results = GuestLoad32(b + 0x10);
    if (!results)
        return E_INVALIDARG_;
    if (size < 8 + 0x5C)
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
    mm::SessionRecord r;
    if (!mm::Find(id, r))
    {
        Log("searchbyid_miss", "XSessionSearchByID(%016llX) -> not found", (unsigned long long)id);
        return X_ONLINE_E_SESSION_NOT_FOUND;
    }
    Log("searchbyid", "XSessionSearchByID(%016llX) -> found", (unsigned long long)id);
    return WriteResults({ r }, results, size);
}

// 0xB001A XSessionArbitrationRegister {obj, flags, nonce, duration, cbResults, pResults}: the local machine.
uint32_t ArbitrationRegister(uint32_t b)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(GuestLoad32(b));
    if (!s)
        return STATUS_INVALID_HANDLE_;
    const uint32_t size = GuestLoad32(b + 0x14), results = GuestLoad32(b + 0x18);
    if (!results)
        return E_INVALIDARG_;
    if (size < 8 + 0x18 + 8)
        return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
    const uint32_t reg = results + 8, users = reg + 0x18;
    GuestStore32(results, 1);
    GuestStore32(results + 4, reg);
    Write64(reg, 0xFA00000000000000ull | (xnet::LocalPeer() & 0xFFFFFFFFFFFFull));
    GuestStore32(reg + 8, 1);
    GuestStore32(reg + 0xC, 1);
    GuestStore32(reg + 0x10, users);
    Write64(users, online::GetIdentity().onlineXuid);
    s->state = State::Registration;
    Log("arbitration", "XSessionArbitrationRegister -> 1 registrant");
    return S_OK_;
}

} // namespace

// ---- session handles ------------------------------------------------------------------------------
// XamSessionCreateHandle(u32* Handle)
SCDA_IMPORT(XamSessionCreateHandle)
{
    const uint32_t out = ctx.r3.u32;
    if (!out)
    {
        ctx.r3.u64 = E_INVALIDARG_;
        return;
    }
    const uint32_t object = GuestAllocVirtual(0x10, 0x10);
    std::lock_guard l(g_mutex);
    const uint32_t handle = g_nextSession++;
    Session s;
    s.guestObject = object;
    g_sessions.emplace(handle, std::move(s));
    GuestStore32(object, handle);
    GuestStore32(out, handle);
    ctx.r3.u64 = 0;
}
// XamSessionRefObjByHandle(Handle, u32* Object)
SCDA_IMPORT(XamSessionRefObjByHandle)
{
    std::lock_guard l(g_mutex);
    const auto it = g_sessions.find(ctx.r3.u32);
    if (it == g_sessions.end())
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE_;
        return;
    }
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, it->second.guestObject);
    ctx.r3.u64 = 0;
}

// ---- XGI dispatch (xam.cpp XMsgStartIORequest, app 0xFB) -----------------------------------------
uint32_t XgiDispatch(uint32_t msg, uint32_t b, uint32_t length, bool& handled)
{
    handled = true;
    switch (msg)
    {
    case 0x000B0006: // XUserSetContext {user, -, xuid, id, value}
    {
        const uint32_t id = GuestLoad32(b + 0x10), value = GuestLoad32(b + 0x14);
        {
            std::lock_guard l(g_mutex);
            g_contexts[id] = value;
        }
        Log("ctx" + std::to_string(id) + "=" + std::to_string(value), "XUserSetContext(%08X = %u)", id, value);
        return S_OK_;
    }
    case 0x000B0007: // XUserSetProperty {user, -, xuid, id, size, data*}
    {
        const uint32_t id = GuestLoad32(b + 0x10), size = GuestLoad32(b + 0x14), data = GuestLoad32(b + 0x18);
        if (data && size <= 0x1000)
        {
            std::lock_guard l(g_mutex);
            g_properties[id].assign(g_base + data, g_base + data + size);
        }
        Log("prop" + std::to_string(id), "XUserSetProperty(%08X, %u bytes)", id, size);
        return S_OK_;
    }
    case 0x000B0008: return S_OK_; // XUserWriteAchievements
    case 0x000B0010: return Create(b);
    case 0x000B0011: // XSessionDelete
    {
        std::lock_guard l(g_mutex);
        Session* s = Lookup(GuestLoad32(b));
        if (!s)
            return STATUS_INVALID_HANDLE_;
        s->state = State::Deleted;
        mm::Remove(s->id);
        Log("delete", "XSessionDelete(%016llX)", (unsigned long long)s->id);
        return S_OK_;
    }
    case 0x000B0012: return JoinLeave(b, true);
    case 0x000B0013: return JoinLeave(b, false);
    case 0x000B0014: return SetState(b, State::InGame, "Start");
    case 0x000B0015: return SetState(b, State::Reporting, "End");
    case 0x000B0016:
    case 0x000B001C: return Search(b);
    case 0x000B0018: // XSessionModify {obj, flags, maxPub, maxPriv}
    {
        std::lock_guard l(g_mutex);
        Session* s = Lookup(GuestLoad32(b));
        if (!s)
            return STATUS_INVALID_HANDLE_;
        s->flags = (s->flags & ~0xF80u) | (GuestLoad32(b + 4) & 0xF80u);
        s->maxPublic = GuestLoad32(b + 8);
        s->maxPrivate = GuestLoad32(b + 0xC);
        Log("modify", "XSessionModify(flags %03X, %u / %u)", s->flags, s->maxPublic, s->maxPrivate);
        Publish(*s);
        return S_OK_;
    }
    case 0x000B001A: return ArbitrationRegister(b);
    case 0x000B001B: return SearchById(b);
    case 0x000B0020: return S_OK_; // XUserResetStatsView
    case 0x000B0025:                // XSessionWriteStats
    case 0x000B0026:                // XSessionFlushStats
        return S_OK_;
    default:
        handled = false;
        Log("unhandled" + std::to_string(msg), "XGI message %08X (length %u) not handled", msg, length);
        return E_FAIL_;
    }
}

// XLiveBase (app 0xFC) 0x00058023 = XInviteGetAcceptedInfo (Xenia xlivebase_app.cc): the invite the player
// accepted (Steam Join Game of a Player Match lobby, xnet.cpp AcceptInvite). XINVITE_INFO (0x54): u64
// invitee, u64 inviter, u32 title id, XSESSION_INFO (0x3C, as the host published it), BOOL fFromGameInvite.
// The message block: u32 user index, XINVITE_INFO* (or the struct itself when the block is that big).
uint32_t XLiveBaseDispatch(uint32_t msg, uint32_t buffer, uint32_t length, bool& handled)
{
    handled = false;
    if (msg != 0x00058023 || !buffer)
        return E_FAIL_;
    handled = true;
    // Conviction (XDK wrapper sub_82FBD5E8): XMsgInProcessCall(0xFC, msg, handle, params). params = 16-byte
    // entries {u32 type 4, pad, u64 value}, count at +512: [0] = &user index, [1] = XINVITE_INFO* (caller stack).
    // The second argument here is that params block; the first is a handle, not the struct.
    uint32_t info = 0;
    const uint32_t params = length;
    if (params >= 0x10000 && params < 0xF0000000u && GuestLoad32(params + 512) == 2 && GuestLoad32(params + 16) == 4)
    {
        const uint32_t p = GuestLoad32(params + 16 + 12); // low word of the u64 value (big-endian)
        if (p >= 0x10000 && p < 0xF0000000u)
            info = p;
    }
    mm::SessionRecord r;
    RuntimeLog("[xsession] XInviteGetAcceptedInfo handle %08X params %08X -> info %08X%s\n", buffer, params, info,
        mm::AcceptedInvite(r) ? "" : " (no accepted invite)");
    if (!info || !mm::AcceptedInvite(r))
        return E_FAIL_;
    std::memset(g_base + info, 0, 0x54);
    Write64(info + 0x00, online::GetIdentity().onlineXuid);
    Write64(info + 0x08, r.hostXuid);
    GuestStore32(info + 0x10, 0x5553080Bu);
    std::memcpy(g_base + info + 0x14, r.info, 0x3C);
    GuestStore32(info + 0x50, 1);
    return S_OK_;
}

// Registered with xam.cpp's XMsgStartIORequest; online play off = not ours (as before: completed empty).
void XamSetXgiHandler(uint32_t (*handler)(uint32_t, uint32_t, uint32_t, bool&));
void XamSetXLiveBaseHandler(uint32_t (*handler)(uint32_t, uint32_t, uint32_t, bool&));
static const bool g_registered = [] {
    XamSetXgiHandler([](uint32_t msg, uint32_t buffer, uint32_t length, bool& handled) -> uint32_t {
        handled = false;
        return xnet::Enabled() ? XgiDispatch(msg, buffer, length, handled) : 0x80004005u;
    });
    XamSetXLiveBaseHandler([](uint32_t msg, uint32_t buffer, uint32_t length, bool& handled) -> uint32_t {
        handled = false;
        return xnet::Enabled() ? XLiveBaseDispatch(msg, buffer, length, handled) : 0x80004005u;
    });
    return true;
}();
