// XNet / WSA housekeeping exports (xam.xex NetDll_*), online step 2. Ported in part from the TTT2 port's
// xlive.cpp; signatures checked against Xenia's xam_net.cc. Every NetDll export takes the caller type in r3
// and its own arguments from r4, except inet_addr, WSAGetLastError, WSASetLastError and the WSA*Event calls.
// Off (xnet::Enabled() false): no network, as the offline runtime always reported.
#include <windows.h>
#include <bcrypt.h>
#include "xnet.h"
#include "net_transport.h"
#include "steam_net.h"
#include <thread>
#include <runtime.h>
#include <settings.h>
#include <online_identity.h>
#include <title.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

void XSocketReceive(uint64_t from, net::Channel channel, const uint8_t* data, size_t size); // xsocket.cpp
void XSocketResendBroadcasts(uint64_t peer);                                                 // xsocket.cpp
extern thread_local uint32_t g_wsaError;                                                  // xsocket.cpp

namespace {
constexpr uint32_t kVirtualNet = 0x0A4E0000u; // 10.78.x.y: never routed, our sockets resolve it
uint32_t OwnAddr(uint64_t peer) { return 0x0A4D0000u | uint32_t(peer & 0xFFFF); } // 10.77.x.y

std::mutex g_peerMutex;
std::map<uint64_t, uint32_t> g_addrOf; // peer id -> virtual address (host order)
std::map<uint32_t, uint64_t> g_peerOf;
std::map<uint64_t, ULONGLONG> g_heard; // peer -> last packet (GetTickCount64)
uint32_t g_next = 1;

// Steam discovery: hosts from the lobby list (owner = the record's session id), hosts of accepted Steam
// "Join Game" invites, and our own hosted lobby.
std::map<uint64_t, ULONGLONG> g_lobbyHosts; // host peer -> when the lobby list last showed it
std::map<uint64_t, ULONGLONG> g_invited;    // host peer -> when the invite arrived
std::atomic<ULONGLONG> g_lastBroadcast{ 0 };
std::atomic<bool> g_hosting{ false };

bool Steam() { return !net::IsLoopbackPeer(net::LocalPeer()) && net::LocalPeer() != 0; }

std::vector<uint8_t> Record(uint64_t self)
{
    std::vector<uint8_t> r = { 'S', 'C', 'C', '1' };
    for (int i = 0; i < 8; i++)
        r.push_back(uint8_t(self >> (8 * i)));
    return r;
}

// Background (Steam only): while a search ran in the last 20 s the lobby list is refreshed every 3 s; new
// hosts get the recent broadcasts. Steam "Join Game" requests are picked up all the time.
void Discovery()
{
    for (;;)
    {
        std::vector<uint8_t> record;
        while (steamnet::TakeJoinRequest(record))
            if (record.size() >= 12 && std::memcmp(record.data(), "SCC1", 4) == 0)
            {
                uint64_t host = 0;
                for (int i = 0; i < 8; i++)
                    host |= uint64_t(record[4 + i]) << (8 * i);
                {
                    std::lock_guard l(g_peerMutex);
                    g_invited[host] = GetTickCount64();
                }
                RuntimeLog("[online] Steam Join Game: host %016llX - its match shows in COOP & MULTIPLAYER > SYSTEM LINK\n", (unsigned long long)host);
            }
        if (GetTickCount64() - g_lastBroadcast.load() < 20000)
        {
            const auto lobbies = steamnet::ListLobbies(50, 4000);
            const uint64_t self = net::LocalPeer();
            std::vector<uint64_t> fresh;
            {
                std::lock_guard l(g_peerMutex);
                for (const auto& r : lobbies)
                {
                    if (r.size() < 12 || std::memcmp(r.data(), "SCC1", 4) != 0)
                        continue;
                    uint64_t host = 0;
                    for (int i = 0; i < 8; i++)
                        host |= uint64_t(r[4 + i]) << (8 * i);
                    static const bool seeOwn = getenv("SCDA_STEAM_SEE_OWN") != nullptr; // test aid: one account, one PC
                    if (host == self && !seeOwn)
                        continue;
                    if (!g_lobbyHosts.count(host))
                        fresh.push_back(host);
                    g_lobbyHosts[host] = GetTickCount64();
                }
            }
            for (const uint64_t host : fresh)
            {
                RuntimeLog("[online] Steam lobby host %016llX found\n", (unsigned long long)host);
                XSocketResendBroadcasts(host);
            }
            Sleep(3000);
        }
        else
            Sleep(500);
    }
}

void Write64(uint32_t a, uint64_t v)
{
    GuestStore32(a, uint32_t(v >> 32));
    GuestStore32(a + 4, uint32_t(v));
}
uint64_t Read64(uint32_t a) { return (uint64_t(GuestLoad32(a)) << 32) | GuestLoad32(a + 4); }

void Log(const char* fmt, ...)
{
    if (!xnet::Trace())
        return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    RuntimeLog("[xnet] %s\n", line);
}

// XNADDR (0x24): ina, inaOnline, wPortOnline, abEnet[6], abOnline[20] (our peer id at +0x18), platform.
void WriteOwnXnAddr(uint32_t a)
{
    memset(g_base + a, 0, 0x24);
    const uint64_t peer = xnet::LocalPeer();
    const uint32_t ina = OwnAddr(peer);
    GuestStore32(a + 0x00, ina);
    GuestStore32(a + 0x04, ina);
    GuestStore16(a + 0x08, 3074);
    for (int i = 0; i < 6; ++i)
        g_base[a + 0x0A + i] = uint8_t(peer >> (8 * i)) | (i == 0 ? 0x02 : 0); // locally administered MAC
    Write64(a + 0x18, peer);
    g_base[a + 0x20] = 1;
}
} // namespace

namespace xnet {

bool Enabled()
{
    // SCDA_ONLINE=0|1 overrides [Online] Enabled (RECOMP SETTINGS > ONLINE, on by default). Scripted
    // runs (SCDA_AUTOPRESS: soaks, A/B tests) stay offline unless SCDA_ONLINE=1.
    static const bool on = [] {
        if (const char* e = getenv("SCDA_ONLINE"))
            return *e == '1';
        if (getenv("SCDA_AUTOPRESS"))
            return false;
        return OnlineEnabledSetting();
    }();
    return on;
}

bool Trace()
{
    static const bool on = getenv("SCDA_NET_TRACE") != nullptr;
    return on;
}

bool EnsureTransport()
{
    if (!Enabled() || !net::Start(XSocketReceive))
        return false;
    static std::once_flag once;
    std::call_once(once, [] {
        // The profile's LIVE identity: the Steam name as gamertag (printable ASCII, 15 at most), or
        // "Player <n>" on loopback (instance n); the online XUID from the peer id.
        online::Identity id;
        id.live = true;
        const uint64_t peer = net::LocalPeer();
        std::string name = ProfileNameSetting(); // [Profile] Name first, then the Steam name
        if (name.empty() && Steam())
            for (char c : steamnet::LocalName())
                if (c >= 0x20 && c < 0x7F && name.size() < 15)
                    name += c;
        if (name.empty())
            name = Steam() ? "Player" : "Player " + std::to_string(int(peer & 0xFFFF) - 28200 + 1);
        id.gamertag = name;
        id.onlineXuid = 0x0009000000000000ull | (peer & 0x0000FFFFFFFFFFFFull);
        online::SetIdentity(id);
        RuntimeLog("[online] signed in to LIVE as \"%s\" (online XUID %016llX)\n", id.gamertag.c_str(), (unsigned long long)id.onlineXuid);
        if (Steam())
            std::thread(Discovery).detach();
    });
    return true;
}

// main.cpp: online play starts before the game (its first sign-in queries see the LIVE identity).
void OnlineStartEarly()
{
    if (Enabled())
        EnsureTransport();
}

void HostStarted()
{
    if (!Enabled() || !EnsureTransport() || g_hosting.exchange(true))
        return;
    RuntimeLog("[online] hosting a System Link match%s\n", Steam() ? ": Steam lobby published" : "");
    if (Steam())
        steamnet::PublishLobby(net::LocalPeer(), Record(net::LocalPeer()));
}

void HostStopped()
{
    if (!g_hosting.exchange(false))
        return;
    RuntimeLog("[online] no longer hosting\n");
    if (Steam())
        steamnet::RemoveLobby(net::LocalPeer());
}

void NoteBroadcast() { g_lastBroadcast = GetTickCount64(); }

uint64_t LocalPeer()
{
    EnsureTransport();
    return net::LocalPeer();
}

void Random(void* out, size_t bytes) { BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

uint32_t AddrOfPeer(uint64_t peer)
{
    if (!peer || peer == net::LocalPeer())
        return OwnAddr(net::LocalPeer());
    std::lock_guard l(g_peerMutex);
    auto it = g_addrOf.find(peer);
    if (it == g_addrOf.end())
    {
        const uint32_t addr = kVirtualNet | g_next++;
        it = g_addrOf.emplace(peer, addr).first;
        g_peerOf.emplace(addr, peer);
    }
    return it->second;
}

uint64_t PeerOfAddr(uint32_t addr)
{
    const uint64_t self = LocalPeer();
    if (addr == 0x7F000001u || addr == OwnAddr(self))
        return self;
    std::lock_guard l(g_peerMutex);
    const auto it = g_peerOf.find(addr);
    return it == g_peerOf.end() ? 0 : it->second;
}

void NoteHeardFrom(uint64_t peer)
{
    std::lock_guard l(g_peerMutex);
    if (g_heard.size() > 64)
        g_heard.clear();
    g_heard[peer] = GetTickCount64();
}

std::vector<uint64_t> LanPeers()
{
    std::vector<uint64_t> out;
    const uint64_t self = LocalPeer();
    if (net::IsLoopbackPeer(self))
        for (int n = 0; n < 8; n++)
            if (net::LoopbackPeer(n) != self)
                out.push_back(net::LoopbackPeer(n));
    std::lock_guard l(g_peerMutex);
    const ULONGLONG now = GetTickCount64();
    auto add = [&](uint64_t peer) {
        if (peer != self && !net::IsLoopbackPeer(peer) && std::find(out.begin(), out.end(), peer) == out.end())
            out.push_back(peer);
    };
    for (const auto& [peer, at] : g_heard)
        if (now - at < 60000)
            add(peer);
    for (const auto& [peer, at] : g_lobbyHosts)
        if (now - at < 60000)
            add(peer);
    for (const auto& [peer, at] : g_invited)
        if (now - at < 600000)
            add(peer);
    return out;
}

} // namespace xnet

// ---- startup -------------------------------------------------------------------------------------
SCDA_IMPORT(NetDll_XNetStartup)
{
    Log("XNetStartup(params %08X)", ctx.r4.u32);
    if (xnet::Enabled())
        xnet::EnsureTransport();
    ctx.r3.u64 = 0;
}
SCDA_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }

// WSAStartup(caller, version, WSADATA*)
SCDA_IMPORT(NetDll_WSAStartup)
{
    const uint32_t version = ctx.r4.u32 & 0xFFFF, data = ctx.r5.u32;
    if (xnet::Enabled() && data)
    {
        memset(g_base + data, 0, 0x190);
        GuestStore16(data, uint16_t(version));
        GuestStore16(data + 2, 0x0202);
    }
    ctx.r3.u64 = 0;
}
SCDA_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }

// ---- WSA events: host events behind kernel handles (handle in r3, no caller) ----------------------
SCDA_IMPORT(NetDll_WSACreateEvent)
{
    ctx.r3.u64 = xnet::Enabled() ? KernelAddHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) : 0;
}
SCDA_IMPORT(NetDll_WSACloseEvent) { ctx.r3.u64 = xnet::Enabled() ? (CloseKernelHandle(ctx.r3.u32) ? 1 : 0) : 0; }
SCDA_IMPORT(NetDll_WSAResetEvent)
{
    HANDLE h = xnet::Enabled() ? KernelHostHandle(ctx.r3.u32) : nullptr;
    ctx.r3.u64 = h && ResetEvent(h) ? 1 : 0;
}
// WSAWaitForMultipleEvents(count, events*, waitAll, timeout ms, alertable) -> WSA_WAIT_EVENT_0 + i,
// WSA_WAIT_TIMEOUT (0x102), WSA_WAIT_FAILED (~0).
SCDA_IMPORT(NetDll_WSAWaitForMultipleEvents)
{
    const uint32_t count = ctx.r3.u32, events = ctx.r4.u32, waitAll = ctx.r5.u32, timeout = ctx.r6.u32, alertable = ctx.r7.u32;
    HANDLE hs[64];
    if (!xnet::Enabled() || !count || count > 64 || !events)
    {
        ctx.r3.u64 = 0xFFFFFFFF;
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        if (!(hs[i] = KernelHostHandle(GuestLoad32(events + 4 * i))))
        {
            ctx.r3.u64 = 0xFFFFFFFF;
            return;
        }
    const DWORD r = TitleWait(count, hs, waitAll != 0, timeout == 0xFFFFFFFF ? INFINITE : timeout, alertable != 0);
    ctx.r3.u64 = r == WAIT_TIMEOUT ? 0x102 : r < WAIT_OBJECT_0 + 64 ? r - WAIT_OBJECT_0 : r == WAIT_IO_COMPLETION ? 0xC0 : 0xFFFFFFFF;
}

// ---- addresses -----------------------------------------------------------------------------------
// XNetGetTitleXnAddr(caller, XNADDR*): ETHERNET|STATIC|GATEWAY|DNS (0x66): a LAN address, no Live.
SCDA_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    const uint32_t addr = ctx.r4.u32;
    if (!xnet::Enabled() || !xnet::EnsureTransport())
    {
        if (addr)
            memset(g_base + addr, 0, 0x24);
        ctx.r3.u64 = 1; // XNET_GET_XNADDR_NONE
        return;
    }
    if (addr)
        WriteOwnXnAddr(addr);
    ctx.r3.u64 = 0x66;
}

// ACTIVE | 100MBPS | FULL_DUPLEX
SCDA_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = xnet::Enabled() && xnet::EnsureTransport() ? 0x0B : 0; }

SCDA_IMPORT(NetDll_XNetRandom)
{
    const uint32_t buffer = ctx.r4.u32, length = ctx.r5.u32;
    if (buffer && length)
        xnet::Random(g_base + buffer, length);
    ctx.r3.u64 = 0;
}

// XNetCreateKey(caller, XNKID*, XNKEY*): a system-link id (high bit pattern of an XNKID_SYSTEM_LINK) and a key.
SCDA_IMPORT(NetDll_XNetCreateKey)
{
    const uint32_t kid = ctx.r4.u32, key = ctx.r5.u32;
    uint8_t id[8];
    xnet::Random(id, 8);
    id[0] = 0x00; // XNET_XNKID_SYSTEM_LINK: top byte 0x00 (online sessions have 0x80+)
    if (kid)
        memcpy(g_base + kid, id, 8);
    if (key)
        xnet::Random(g_base + key, 16);
    Log("XNetCreateKey");
    ctx.r3.u64 = 0;
}
SCDA_IMPORT(NetDll_XNetRegisterKey)
{
    Log("XNetRegisterKey(%016llX)", (unsigned long long)(ctx.r4.u32 ? Read64(ctx.r4.u32) : 0));
    ctx.r3.u64 = 0;
}
SCDA_IMPORT(NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }

// XNetXnAddrToInAddr(caller, XNADDR*, XNKID*, IN_ADDR*): each remote peer its own 10.78.x.y; our own
// XNADDR is 127.0.0.1 (as Xenia; titles connect to themselves).
SCDA_IMPORT(NetDll_XNetXnAddrToInAddr)
{
    const uint32_t xna = ctx.r4.u32, out = ctx.r6.u32;
    if (!xnet::Enabled() || !xna || !out)
    {
        ctx.r3.u64 = 10022; // WSAEINVAL
        return;
    }
    const uint64_t peer = Read64(xna + 0x18);
    const uint32_t ina = (!peer || peer == xnet::LocalPeer()) ? 0x7F000001u : xnet::AddrOfPeer(peer);
    GuestStore32(out, ina);
    Log("XNetXnAddrToInAddr(peer %016llX) -> %u.%u.%u.%u", (unsigned long long)peer, ina >> 24, (ina >> 16) & 0xFF, (ina >> 8) & 0xFF, ina & 0xFF);
    ctx.r3.u64 = 0;
}

// XNetInAddrToXnAddr is not imported by Conviction. XNetInAddrToString(caller, IN_ADDR, char*, size).
SCDA_IMPORT(NetDll_XNetInAddrToString)
{
    const uint32_t ina = ctx.r4.u32, buf = ctx.r5.u32, size = ctx.r6.u32;
    if (buf && size)
    {
        char s[20];
        snprintf(s, sizeof(s), "%u.%u.%u.%u", ina >> 24, (ina >> 16) & 0xFF, (ina >> 8) & 0xFF, ina & 0xFF);
        strncpy(reinterpret_cast<char*>(g_base + buf), s, size);
        g_base[buf + size - 1] = 0;
    }
    ctx.r3.u64 = 0;
}

// XNetXnAddrToMachineId(caller, XNADDR*, u64*): the peer id.
SCDA_IMPORT(NetDll_XNetXnAddrToMachineId)
{
    const uint32_t xna = ctx.r4.u32, out = ctx.r5.u32;
    if (out)
        Write64(out, xnet::Enabled() && xna ? Read64(xna + 0x18) : 0);
    ctx.r3.u64 = xnet::Enabled() ? 0 : 10050;
}

SCDA_IMPORT(NetDll_XNetConnect) { ctx.r3.u64 = xnet::Enabled() ? 0 : 10050; }
// XNET_CONNECT_STATUS_CONNECTED (2) for a known peer, LOST (3) otherwise.
SCDA_IMPORT(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = xnet::Enabled() && xnet::PeerOfAddr(ctx.r4.u32) ? 2 : 3; }
SCDA_IMPORT(NetDll_XNetUnregisterInAddr) { ctx.r3.u64 = 0; }
SCDA_IMPORT(NetDll_XNetServerToInAddr) { ctx.r3.u64 = 10065; } // WSAEHOSTUNREACH: no Live servers
SCDA_IMPORT(NetDll_XNetSetSystemLinkPort)
{
    Log("XNetSetSystemLinkPort(%u)", ctx.r4.u32 & 0xFFFF);
    ctx.r3.u64 = 0;
}

// DNS (Live / Uplay servers): none.
SCDA_IMPORT(NetDll_XNetDnsLookup)
{
    Log("XNetDnsLookup(%s)", ctx.r4.u32 ? reinterpret_cast<const char*>(g_base + ctx.r4.u32) : "");
    if (ctx.r6.u32)
        GuestStore32(ctx.r6.u32, 0);
    ctx.r3.u64 = 11001; // WSAHOST_NOT_FOUND
}
SCDA_IMPORT(NetDll_XNetDnsRelease) { ctx.r3.u64 = 0; }

// QoS (host probing): step 3. Listen succeeds; lookups are traced and refused for now.
SCDA_IMPORT(NetDll_XNetQosListen)
{
    Log("XNetQosListen(kid %08X data %08X size %u flags %X)", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r8.u32);
    ctx.r3.u64 = 0;
}
SCDA_IMPORT(NetDll_XNetQosLookup)
{
    Log("XNetQosLookup(cxna %u, ...) - not yet", ctx.r4.u32);
    ctx.r3.u64 = 10050;
}
SCDA_IMPORT(NetDll_XNetQosRelease) { ctx.r3.u64 = 0; }
