// Virtual Xbox sockets (online step 2, ported from the TTT2 port's xsocket.cpp): the game's NetDll socket
// API on top of the packet transport (net_transport.h) - no host sockets for game traffic. Addresses are
// the virtual IN_ADDRs of xnet.h (a remote peer 10.78.x.y; our own 10.77.x.y, also 127.0.0.1).
//  - datagrams (UDP 17 / VDP 254, payload untouched): one packet each, unreliable channel. A LAN broadcast
//    (255.255.255.255 or x.x.x.255) goes to every LAN peer (xnet::LanPeers: System Link discovery);
//  - streams (TCP): SYN / SYN-ACK / DATA / FIN / RST on the reliable channel, a connection is
//    (peer, connection id, direction);
//  - overlapped WSARecvFrom / WSASendTo (Conviction's Quazal layer): a receive without data pends
//    (WSA_IO_PENDING) and completes when a datagram arrives: buffers filled, OVERLAPPED {Internal 0,
//    InternalHigh bytes} and its event (+0x10) set. Sends complete at once.
// Guest structs are big-endian: XSOCKADDR_IN {u16 family, u16 port, u32 addr, 8 zero},
// fd_set {u32 count, u32 handles[64]}, timeval {s32 sec, s32 usec}, WSABUF {u32 len, u32 buf},
// WSAOVERLAPPED {u32 Internal, u32 InternalHigh, u32 Offset, u32 OffsetHigh, u32 hEvent}.
// Off (xnet::Enabled() false): every socket call fails with WSAENETDOWN, as before.
// SCDA_NET_TRACE=1 logs every socket call and packet (=2: whole payloads up to 2 KB).
#include <windows.h>
#include "xnet.h"
#include "net_transport.h"
#include <runtime.h>
#include <title.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

thread_local uint32_t g_wsaError = 0;

namespace {

constexpr uint32_t WSAEWOULDBLOCK_ = 10035, WSAENOTSOCK_ = 10038, WSAEINVAL_ = 10022, WSAEFAULT_ = 10014;
constexpr uint32_t WSAEADDRINUSE_ = 10048, WSAEISCONN_ = 10056, WSAENOTCONN_ = 10057, WSAECONNREFUSED_ = 10061;
constexpr uint32_t WSAECONNRESET_ = 10054, WSAETIMEDOUT_ = 10060, WSAEHOSTUNREACH_ = 10065, WSAEMSGSIZE_ = 10040;
constexpr uint32_t WSAEPROTONOSUPPORT_ = 10043, WSAEOPNOTSUPP_ = 10045, WSAENETDOWN_ = 10050;
constexpr uint32_t WSA_IO_PENDING_ = 997, WSA_IO_INCOMPLETE_ = 996;
constexpr uint32_t kStatusPending = 0x103;

enum class Kind : uint8_t { Stream, Datagram };
enum class TcpState : uint8_t { Idle, Listening, Connecting, Connected, PeerClosed, Refused };

struct Datagram { uint32_t fromAddr; uint16_t fromPort; std::vector<uint8_t> data; };
// An overlapped WSARecvFrom waiting for a datagram (guest pointers).
struct PendingRecv { uint32_t buffers, count, bytes, flags, from, fromLen, overlapped; };
struct Socket
{
    Kind kind = Kind::Datagram;
    uint8_t proto = 17;
    bool nonBlocking = false, bound = false, reuse = false, connector = false, sendShut = false, broadcast = false;
    uint16_t port = 0;
    TcpState state = TcpState::Idle;
    uint64_t peer = 0;
    uint16_t peerPort = 0;
    uint32_t connId = 0;
    uint32_t pendingError = 0;
    std::deque<uint8_t> stream;
    std::deque<uint32_t> accepted;
    std::deque<Datagram> datagrams;
    std::deque<PendingRecv> pendingRecvs;
    std::map<uint32_t, uint32_t> options;
};

std::mutex g_m;
std::condition_variable g_cv;
struct SentBroadcast { ULONGLONG at; uint8_t proto; uint16_t srcPort, dstPort; std::vector<uint8_t> data; };
std::deque<SentBroadcast> g_broadcasts; // under g_m
std::map<uint32_t, Socket> g_sockets;
uint32_t g_nextHandle = 0xD4000000u;
uint16_t g_nextEphemeral = 49152;

int Fail(uint32_t error)
{
    g_wsaError = error;
    return -1;
}

// ---- wire format ----
enum class Op : uint8_t { Datagram = 1, Syn = 2, SynAck = 3, Reset = 4, Data = 5, Fin = 6 };
#pragma pack(push, 1)
struct Header { Op op; uint8_t proto; uint8_t toConnector; uint8_t pad; uint16_t srcPort, dstPort; uint32_t connId; };
#pragma pack(pop)

bool SendPacket(uint64_t peer, net::Channel channel, Op op, uint8_t proto, bool toConnector, uint16_t src, uint16_t dst, uint32_t connId,
                const uint8_t* data, size_t size)
{
    thread_local std::vector<uint8_t> packet;
    packet.resize(sizeof(Header) + size);
    Header h{ op, proto, uint8_t(toConnector), 0, src, dst, connId };
    std::memcpy(packet.data(), &h, sizeof(h));
    if (size)
        std::memcpy(packet.data() + sizeof(h), data, size);
    return net::Send(peer, channel, packet.data(), packet.size());
}

Socket* Find(uint32_t handle)
{
    const auto it = g_sockets.find(handle);
    return it == g_sockets.end() ? nullptr : &it->second;
}

void TraceBytes(const char* what, uint32_t handle, uint32_t addr, uint16_t port, const uint8_t* data, int n)
{
    if (!xnet::Trace())
        return;
    static const int limit = [] {
        const char* e = getenv("SCDA_NET_TRACE");
        return e && *e == '2' ? 2048 : 24;
    }();
    char hex[4100] = {};
    int at = 0;
    for (int i = 0; i < n && i < limit; ++i)
        at += snprintf(hex + at, sizeof(hex) - at, "%02X", data[i]);
    RuntimeLog("[xsocket] %s %08X %u.%u.%u.%u:%u %d bytes %s\n", what, handle, addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
               port, n, hex);
}

uint16_t Ephemeral()
{
    for (;;)
    {
        const uint16_t p = g_nextEphemeral++;
        if (g_nextEphemeral == 0)
            g_nextEphemeral = 49152;
        bool used = false;
        for (auto& [h, s] : g_sockets)
            used |= s.bound && s.port == p;
        if (!used)
            return p;
    }
}

void ReadAddr(uint32_t sa, uint32_t& addr, uint16_t& port)
{
    port = GuestLoad16(sa + 2);
    addr = GuestLoad32(sa + 4);
}
void WriteAddr(uint32_t sa, uint32_t lenPtr, uint32_t addr, uint16_t port)
{
    if (!sa)
        return;
    std::memset(g_base + sa, 0, 16);
    GuestStore16(sa, 2);
    GuestStore16(sa + 2, port);
    GuestStore32(sa + 4, addr);
    if (lenPtr)
        GuestStore32(lenPtr, 16);
}

bool IsBroadcast(uint32_t addr) { return addr == 0xFFFFFFFFu || (addr & 0xFF) == 0xFF; }

bool Readable(const Socket& s)
{
    if (s.kind == Kind::Datagram)
        return !s.datagrams.empty();
    if (s.state == TcpState::Listening)
        return !s.accepted.empty();
    return !s.stream.empty() || s.state == TcpState::PeerClosed || s.state == TcpState::Refused;
}
bool Writable(const Socket& s) { return s.kind == Kind::Datagram || s.state == TcpState::Connected; }
bool Excepted(const Socket& s) { return s.kind == Kind::Stream && s.state == TcpState::Refused; }

// Datagram d into a pending overlapped receive (under g_m): the buffers, from, byte count, OVERLAPPED, event.
void CompleteRecv(const PendingRecv& r, const Datagram& d)
{
    uint32_t done = 0;
    for (uint32_t i = 0; i < r.count && done < d.data.size(); i++)
    {
        const uint32_t len = GuestLoad32(r.buffers + 8 * i), buf = GuestLoad32(r.buffers + 8 * i + 4);
        const uint32_t n = std::min<uint32_t>(len, uint32_t(d.data.size()) - done);
        std::memcpy(g_base + buf, d.data.data() + done, n);
        done += n;
    }
    WriteAddr(r.from, r.fromLen, d.fromAddr, d.fromPort);
    if (r.bytes)
        GuestStore32(r.bytes, done);
    if (r.flags)
        GuestStore32(r.flags, 0);
    if (r.overlapped)
    {
        GuestStore32(r.overlapped + 0, done < d.data.size() ? 0x80000005 : 0); // STATUS_BUFFER_OVERFLOW when truncated
        GuestStore32(r.overlapped + 4, done);
        if (HANDLE e = KernelHostHandle(GuestLoad32(r.overlapped + 0x10)))
            SetEvent(e);
    }
}

} // namespace

// A host the Steam lobby search found after a System Link search went out: the recent broadcasts, to it.
void XSocketResendBroadcasts(uint64_t peer)
{
    std::lock_guard l(g_m);
    const ULONGLONG now = GetTickCount64();
    for (const SentBroadcast& b : g_broadcasts)
        if (now - b.at <= 6000)
            SendPacket(peer, net::Channel::Unreliable, Op::Datagram, b.proto, false, b.srcPort, b.dstPort, 0, b.data.data(), b.data.size());
}

// ---- incoming packets (transport thread) ----
void XSocketReceive(uint64_t from, net::Channel, const uint8_t* data, size_t size)
{
    if (size < sizeof(Header))
        return;
    xnet::NoteHeardFrom(from);
    Header h;
    std::memcpy(&h, data, sizeof(h));
    const uint8_t* payload = data + sizeof(h);
    const size_t payloadSize = size - sizeof(h);
    const uint32_t fromAddr = xnet::AddrOfPeer(from);
    std::lock_guard l(g_m);
    if (xnet::Trace())
        RuntimeLog("[xsocket] rx op %u proto %u %u->%u conn %08X %zu bytes from %016llX\n", unsigned(h.op), h.proto, h.srcPort, h.dstPort, h.connId,
                   payloadSize, (unsigned long long)from);
    switch (h.op)
    {
    case Op::Datagram:
    {
        bool delivered = false;
        for (auto& [handle, s] : g_sockets)
            if (s.kind == Kind::Datagram && s.bound && s.port == h.dstPort && (s.proto == h.proto || h.proto == 17 || s.proto == 17))
            {
                Datagram d{ fromAddr, h.srcPort, std::vector<uint8_t>(payload, payload + payloadSize) };
                if (!s.pendingRecvs.empty())
                {
                    CompleteRecv(s.pendingRecvs.front(), d);
                    s.pendingRecvs.pop_front();
                    delivered = true;
                }
                else if (s.datagrams.size() < 512)
                {
                    s.datagrams.push_back(std::move(d));
                    delivered = true;
                }
                break;
            }
        if (!delivered && xnet::Trace())
            RuntimeLog("[xsocket] rx datagram DROPPED: no socket bound to proto %u port %u (or queue full)\n", h.proto, h.dstPort);
        break;
    }
    case Op::Syn:
    {
        Socket* listener = nullptr;
        for (auto& [handle, s] : g_sockets)
            if (s.kind == Kind::Stream && s.state == TcpState::Listening && s.port == h.dstPort)
            {
                listener = &s;
                break;
            }
        if (!listener || listener->accepted.size() >= 16)
        {
            SendPacket(from, net::Channel::Reliable, Op::Reset, h.proto, true, h.dstPort, h.srcPort, h.connId, nullptr, 0);
            break;
        }
        Socket c;
        c.kind = Kind::Stream;
        c.proto = 6;
        c.bound = true;
        c.port = h.dstPort;
        c.state = TcpState::Connected;
        c.peer = from;
        c.peerPort = h.srcPort;
        c.connId = h.connId;
        const uint32_t handle = g_nextHandle++;
        g_sockets.emplace(handle, std::move(c)); // map nodes are stable: `listener` stays valid
        listener->accepted.push_back(handle);
        SendPacket(from, net::Channel::Reliable, Op::SynAck, 6, true, h.dstPort, h.srcPort, h.connId, nullptr, 0);
        break;
    }
    default:
        for (auto& [handle, s] : g_sockets)
        {
            if (s.kind != Kind::Stream || s.peer != from || s.connId != h.connId || s.connector != bool(h.toConnector))
                continue;
            if (h.op == Op::SynAck && s.state == TcpState::Connecting)
                s.state = TcpState::Connected;
            else if (h.op == Op::Reset)
            {
                s.state = s.state == TcpState::Connecting ? TcpState::Refused : TcpState::PeerClosed;
                s.pendingError = WSAECONNRESET_;
            }
            else if (h.op == Op::Data)
                s.stream.insert(s.stream.end(), payload, payload + payloadSize);
            else if (h.op == Op::Fin)
                s.state = TcpState::PeerClosed;
            break;
        }
        break;
    }
    g_cv.notify_all();
}

namespace {
template <typename F> bool WaitFor(std::unique_lock<std::mutex>& lock, F ready, int timeoutMs = 30000)
{
    return g_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
}

// A datagram to `to` (or the socket's connected peer): one packet; a broadcast to every LAN peer.
int SendDatagram(Socket& s, uint32_t handle, uint32_t to, const uint8_t* data, uint32_t length)
{
    uint64_t peer = s.peer;
    uint16_t port = s.peerPort;
    uint32_t addr = 0;
    if (to)
    {
        ReadAddr(to, addr, port);
        peer = IsBroadcast(addr) ? 0 : xnet::PeerOfAddr(addr);
    }
    if (length > 1400)
        return Fail(WSAEMSGSIZE_);
    if (!s.bound)
    {
        s.port = Ephemeral();
        s.bound = true;
    }
    if (to && IsBroadcast(addr))
    {
        xnet::NoteBroadcast();
        {
            // Kept 6 s for hosts the Steam lobby search finds later (XSocketResendBroadcasts).
            const ULONGLONG now = GetTickCount64();
            while (!g_broadcasts.empty() && (g_broadcasts.size() >= 8 || now - g_broadcasts.front().at > 6000))
                g_broadcasts.pop_front();
            g_broadcasts.push_back({ now, s.proto, s.port, port, std::vector<uint8_t>(data, data + length) });
        }
        const auto peers = xnet::LanPeers();
        for (const uint64_t p : peers)
            SendPacket(p, net::Channel::Unreliable, Op::Datagram, s.proto, false, s.port, port, 0, data, length);
        TraceBytes("broadcast", handle, addr, port, data, int(length));
        return int(length);
    }
    if (!peer)
    {
        TraceBytes("sendto(unknown address, dropped)", handle, addr, port, data, int(length));
        return int(length); // unknown address: lost, as with UDP
    }
    if (peer == xnet::LocalPeer())
    {
        // To ourselves (127.0.0.1 / our own address): straight into our own sockets.
        thread_local std::vector<uint8_t> packet;
        packet.resize(sizeof(Header) + length);
        Header h{ Op::Datagram, s.proto, 0, 0, s.port, port, 0 };
        std::memcpy(packet.data(), &h, sizeof(h));
        std::memcpy(packet.data() + sizeof(h), data, length);
        g_m.unlock();
        XSocketReceive(peer, net::Channel::Unreliable, packet.data(), packet.size());
        g_m.lock();
        return int(length);
    }
    SendPacket(peer, net::Channel::Unreliable, Op::Datagram, s.proto, false, s.port, port, 0, data, length);
    TraceBytes("sendto", handle, xnet::AddrOfPeer(peer), port, data, int(length));
    return int(length);
}

// The guest buffers (WSABUF list) as one block.
std::vector<uint8_t> Gather(uint32_t buffers, uint32_t count)
{
    std::vector<uint8_t> out;
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        const uint32_t len = GuestLoad32(buffers + 8 * i), buf = GuestLoad32(buffers + 8 * i + 4);
        out.insert(out.end(), g_base + buf, g_base + buf + len);
    }
    return out;
}
} // namespace

#define OFFLINE_FAIL()                                                                                                                     \
    if (!xnet::Enabled())                                                                                                                  \
    {                                                                                                                                      \
        ctx.r3.u64 = uint32_t(Fail(WSAENETDOWN_));                                                                                         \
        return;                                                                                                                            \
    }

// ---- the NetDll socket API (r3 = caller) ----------------------------------------------------------
SCDA_IMPORT(NetDll_socket)
{
    OFFLINE_FAIL();
    const uint32_t af = ctx.r4.u32, type = ctx.r5.u32, protocol = ctx.r6.u32;
    xnet::EnsureTransport();
    Socket s;
    if (af != 2)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEPROTONOSUPPORT_));
        return;
    }
    if (type == 1 || (type == 0 && protocol == 6))
    {
        s.kind = Kind::Stream;
        s.proto = 6;
    }
    else if (type == 2 || (type == 0 && (protocol == 17 || protocol == 254)))
    {
        s.kind = Kind::Datagram;
        s.proto = protocol == 254 ? 254 : 17;
    }
    else
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEPROTONOSUPPORT_));
        return;
    }
    std::lock_guard l(g_m);
    const uint32_t handle = g_nextHandle++;
    g_sockets.emplace(handle, std::move(s));
    if (xnet::Trace())
        RuntimeLog("[xsocket] socket(type %u, proto %u) -> %08X\n", type, protocol, handle);
    ctx.r3.u64 = handle;
}

SCDA_IMPORT(NetDll_closesocket)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (s->kind == Kind::Stream && s->state == TcpState::Connected && !s->sendShut)
        SendPacket(s->peer, net::Channel::Reliable, Op::Fin, 6, !s->connector, s->port, s->peerPort, s->connId, nullptr, 0);
    // Pending overlapped receives end (STATUS_CANCELLED) with their events set.
    for (const PendingRecv& r : s->pendingRecvs)
        if (r.overlapped)
        {
            GuestStore32(r.overlapped, 0xC0000120);
            if (HANDLE e = KernelHostHandle(GuestLoad32(r.overlapped + 0x10)))
                SetEvent(e);
        }
    for (const uint32_t pending : s->accepted)
        g_sockets.erase(pending);
    if (xnet::Trace())
        RuntimeLog("[xsocket] closesocket %08X\n", ctx.r4.u32);
    g_sockets.erase(ctx.r4.u32);
    g_cv.notify_all();
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(NetDll_bind)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (!ctx.r5.u32 || ctx.r6.u32 < 16)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEFAULT_));
        return;
    }
    uint32_t addr = 0;
    uint16_t port = 0;
    ReadAddr(ctx.r5.u32, addr, port);
    if (!port)
        port = Ephemeral();
    for (auto& [h, o] : g_sockets)
        if (&o != s && o.bound && o.port == port && o.kind == s->kind && o.proto == s->proto && !(s->reuse && o.reuse))
        {
            ctx.r3.u64 = uint32_t(Fail(WSAEADDRINUSE_));
            return;
        }
    s->port = port;
    s->bound = true;
    if (xnet::Trace())
        RuntimeLog("[xsocket] bind %08X proto %u port %u\n", ctx.r4.u32, s->proto, port);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(NetDll_listen)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (s->kind != Kind::Stream || !s->bound)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEINVAL_));
        return;
    }
    s->state = TcpState::Listening;
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(NetDll_accept)
{
    OFFLINE_FAIL();
    const uint32_t handle = ctx.r4.u32, sa = ctx.r5.u32, lenPtr = ctx.r6.u32;
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (s->state != TcpState::Listening)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEINVAL_));
        return;
    }
    if (s->accepted.empty())
    {
        if (s->nonBlocking)
        {
            ctx.r3.u64 = uint32_t(Fail(WSAEWOULDBLOCK_));
            return;
        }
        WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->accepted.empty(); }, 0x7FFFFFFF);
        s = Find(handle);
        if (!s || s->accepted.empty())
        {
            ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
            return;
        }
    }
    const uint32_t child = s->accepted.front();
    s->accepted.pop_front();
    if (Socket* c = Find(child))
        WriteAddr(sa, lenPtr, xnet::AddrOfPeer(c->peer), c->peerPort);
    ctx.r3.u64 = child;
}

SCDA_IMPORT(NetDll_connect)
{
    OFFLINE_FAIL();
    const uint32_t handle = ctx.r4.u32;
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (!ctx.r5.u32)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEFAULT_));
        return;
    }
    uint32_t addr = 0;
    uint16_t port = 0;
    ReadAddr(ctx.r5.u32, addr, port);
    const uint64_t peer = xnet::PeerOfAddr(addr);
    if (!peer)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEHOSTUNREACH_));
        return;
    }
    if (!s->bound)
    {
        s->port = Ephemeral();
        s->bound = true;
    }
    if (s->kind == Kind::Datagram)
    {
        s->peer = peer; // a default destination
        s->peerPort = port;
        ctx.r3.u64 = 0;
        return;
    }
    if (s->state == TcpState::Connected)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEISCONN_));
        return;
    }
    s->peer = peer;
    s->peerPort = port;
    xnet::Random(&s->connId, sizeof(s->connId));
    s->connector = true;
    s->state = TcpState::Connecting;
    SendPacket(peer, net::Channel::Reliable, Op::Syn, 6, false, s->port, port, s->connId, nullptr, 0);
    if (xnet::Trace())
        RuntimeLog("[xsocket] connect %08X -> peer %016llX:%u\n", handle, (unsigned long long)peer, port);
    if (s->nonBlocking)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEWOULDBLOCK_));
        return;
    }
    const bool done = WaitFor(l, [&] { Socket* x = Find(handle); return !x || x->state != TcpState::Connecting; }, 10000);
    s = Find(handle);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (!done)
    {
        s->state = TcpState::Refused;
        ctx.r3.u64 = uint32_t(Fail(WSAETIMEDOUT_));
        return;
    }
    ctx.r3.u64 = s->state == TcpState::Connected ? 0 : uint32_t(Fail(WSAECONNREFUSED_));
}

// send / sendto: streams go out as reliable DATA chunks, datagrams as one unreliable packet.
static int SendCommon(uint32_t handle, const uint8_t* data, uint32_t length, uint32_t to)
{
    std::lock_guard l(g_m);
    Socket* s = Find(handle);
    if (!s)
        return Fail(WSAENOTSOCK_);
    if (s->kind == Kind::Stream)
    {
        if (s->state != TcpState::Connected || s->sendShut)
            return Fail(WSAENOTCONN_);
        TraceBytes("send", handle, xnet::AddrOfPeer(s->peer), s->peerPort, data, int(length));
        for (uint32_t done = 0; done < length;)
        {
            const uint32_t chunk = (std::min)(length - done, 16384u);
            SendPacket(s->peer, net::Channel::Reliable, Op::Data, 6, !s->connector, s->port, s->peerPort, s->connId, data + done, chunk);
            done += chunk;
        }
        return int(length);
    }
    return SendDatagram(*s, handle, to, data, length);
}
SCDA_IMPORT(NetDll_send)
{
    OFFLINE_FAIL();
    ctx.r3.u64 = uint32_t(SendCommon(ctx.r4.u32, g_base + ctx.r5.u32, ctx.r6.u32, 0));
}
// sendto(caller, s, buffer, length, flags, to, tolen)
SCDA_IMPORT(NetDll_sendto)
{
    OFFLINE_FAIL();
    ctx.r3.u64 = uint32_t(SendCommon(ctx.r4.u32, g_base + ctx.r5.u32, ctx.r6.u32, ctx.r8.u32));
}

// WSASendTo(caller, s, WSABUF*, count, bytesSent*, flags, to, tolen, [r1+84] overlapped, [r1+92] completion):
// the buffers as one datagram (VDP: buffer 0 = game data length, 1 = game data; passed through), done at once.
SCDA_IMPORT(NetDll_WSASendTo)
{
    OFFLINE_FAIL();
    const uint32_t handle = ctx.r4.u32, buffers = ctx.r5.u32, count = ctx.r6.u32, sent = ctx.r7.u32, to = ctx.r9.u32;
    const uint32_t overlapped = GuestLoad32(ctx.r1.u32 + 84);
    const std::vector<uint8_t> data = Gather(buffers, count);
    const int n = SendCommon(handle, data.data(), uint32_t(data.size()), to);
    if (n < 0)
    {
        ctx.r3.u64 = 0xFFFFFFFF;
        return;
    }
    if (sent)
        GuestStore32(sent, uint32_t(n));
    if (overlapped)
    {
        GuestStore32(overlapped + 0, 0);
        GuestStore32(overlapped + 4, uint32_t(n));
        if (HANDLE e = KernelHostHandle(GuestLoad32(overlapped + 0x10)))
            SetEvent(e);
    }
    ctx.r3.u64 = 0;
}

// recv / recvfrom
static int RecvCommon(uint32_t handle, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from, uint32_t fromLen)
{
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s)
        return Fail(WSAENOTSOCK_);
    const bool peek = flags & 2;
    if (s->kind == Kind::Datagram)
    {
        if (s->datagrams.empty())
        {
            if (s->nonBlocking)
                return Fail(WSAEWOULDBLOCK_);
            WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->datagrams.empty(); }, 0x7FFFFFFF);
            s = Find(handle);
            if (!s || s->datagrams.empty())
                return Fail(WSAENOTSOCK_);
        }
        Datagram& d = s->datagrams.front();
        const uint32_t n = (std::min)(length, uint32_t(d.data.size()));
        if (n)
            std::memcpy(g_base + buffer, d.data.data(), n);
        WriteAddr(from, fromLen, d.fromAddr, d.fromPort);
        TraceBytes("recvfrom", handle, d.fromAddr, d.fromPort, d.data.data(), int(d.data.size()));
        const bool truncated = d.data.size() > length;
        if (!peek)
            s->datagrams.pop_front();
        return truncated ? Fail(WSAEMSGSIZE_) : int(n);
    }
    if (s->state == TcpState::Listening || s->state == TcpState::Idle || s->state == TcpState::Connecting)
        return Fail(s->state == TcpState::Connecting && s->nonBlocking ? WSAEWOULDBLOCK_ : WSAENOTCONN_);
    if (s->stream.empty())
    {
        if (s->state == TcpState::PeerClosed)
            return s->pendingError ? Fail(WSAECONNRESET_) : 0;
        if (s->state == TcpState::Refused)
            return Fail(WSAECONNRESET_);
        if (s->nonBlocking)
            return Fail(WSAEWOULDBLOCK_);
        WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->stream.empty() || x->state != TcpState::Connected; }, 0x7FFFFFFF);
        s = Find(handle);
        if (!s)
            return Fail(WSAENOTSOCK_);
        if (s->stream.empty())
            return s->state == TcpState::PeerClosed && !s->pendingError ? 0 : Fail(WSAECONNRESET_);
    }
    const uint32_t n = (std::min)(length, uint32_t(s->stream.size()));
    std::copy(s->stream.begin(), s->stream.begin() + n, g_base + buffer);
    if (!peek)
        s->stream.erase(s->stream.begin(), s->stream.begin() + n);
    WriteAddr(from, fromLen, xnet::AddrOfPeer(s->peer), s->peerPort);
    return int(n);
}
SCDA_IMPORT(NetDll_recv)
{
    OFFLINE_FAIL();
    ctx.r3.u64 = uint32_t(RecvCommon(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0));
}
// recvfrom(caller, s, buffer, length, flags, from, fromlen)
SCDA_IMPORT(NetDll_recvfrom)
{
    OFFLINE_FAIL();
    if (ctx.r8.u32)
    {
        std::memset(g_base + ctx.r8.u32, 0, 16);
        GuestStore16(ctx.r8.u32, 2);
    }
    ctx.r3.u64 = uint32_t(RecvCommon(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32));
}

// WSARecvFrom(caller, s, WSABUF*, count, bytes*, flags*, from, fromlen*, [r1+84] overlapped, [r1+92] completion)
SCDA_IMPORT(NetDll_WSARecvFrom)
{
    OFFLINE_FAIL();
    PendingRecv r{ ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, GuestLoad32(ctx.r1.u32 + 84) };
    const uint32_t handle = ctx.r4.u32;
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s || s->kind != Kind::Datagram)
    {
        ctx.r3.u64 = uint32_t(Fail(s ? WSAEOPNOTSUPP_ : WSAENOTSOCK_));
        return;
    }
    if (!s->datagrams.empty())
    {
        Datagram d = std::move(s->datagrams.front());
        s->datagrams.pop_front();
        CompleteRecv(r, d);
        ctx.r3.u64 = 0;
        return;
    }
    if (r.overlapped)
    {
        GuestStore32(r.overlapped, kStatusPending);
        GuestStore32(r.overlapped + 4, 0);
        if (HANDLE e = KernelHostHandle(GuestLoad32(r.overlapped + 0x10)))
            ResetEvent(e);
        s->pendingRecvs.push_back(r);
        ctx.r3.u64 = uint32_t(Fail(WSA_IO_PENDING_));
        return;
    }
    if (s->nonBlocking)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEWOULDBLOCK_));
        return;
    }
    WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->datagrams.empty(); }, 0x7FFFFFFF);
    s = Find(handle);
    if (!s || s->datagrams.empty())
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    Datagram d = std::move(s->datagrams.front());
    s->datagrams.pop_front();
    CompleteRecv(r, d);
    ctx.r3.u64 = 0;
}

// WSAGetOverlappedResult(caller, s, overlapped, bytes*, wait, flags*) -> TRUE when done.
SCDA_IMPORT(NetDll_WSAGetOverlappedResult)
{
    OFFLINE_FAIL();
    const uint32_t ov = ctx.r5.u32, bytes = ctx.r6.u32, wait = ctx.r7.u32, flags = ctx.r8.u32;
    if (!ov)
    {
        g_wsaError = WSAEFAULT_;
        ctx.r3.u64 = 0;
        return;
    }
    if (GuestLoad32(ov) == kStatusPending)
    {
        if (!wait)
        {
            g_wsaError = WSA_IO_INCOMPLETE_;
            ctx.r3.u64 = 0;
            return;
        }
        if (HANDLE e = KernelHostHandle(GuestLoad32(ov + 0x10)))
            TitleWait(1, &e, false, INFINITE, false);
    }
    const uint32_t status = GuestLoad32(ov);
    if (bytes)
        GuestStore32(bytes, GuestLoad32(ov + 4));
    if (flags)
        GuestStore32(flags, 0);
    if (status && status != 0x80000005)
    {
        g_wsaError = status == 0xC0000120 ? 995 /* WSA_OPERATION_ABORTED */ : WSAECONNRESET_;
        ctx.r3.u64 = 0;
        return;
    }
    if (status == 0x80000005)
        g_wsaError = WSAEMSGSIZE_;
    ctx.r3.u64 = status ? 0 : 1;
}

// select(caller, nfds, read, write, except, timeout): handles are ours; ready ones are kept.
SCDA_IMPORT(NetDll_select)
{
    OFFLINE_FAIL();
    const uint32_t sets[3] = { ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 }, timeout = ctx.r8.u32;
    int64_t waitMs = -1;
    if (timeout)
        waitMs = int64_t(int32_t(GuestLoad32(timeout))) * 1000 + int32_t(GuestLoad32(timeout + 4)) / 1000;
    std::vector<uint32_t> want[3];
    for (int k = 0; k < 3; ++k)
        if (sets[k])
        {
            const uint32_t n = (std::min)(GuestLoad32(sets[k]), 64u);
            for (uint32_t i = 0; i < n; ++i)
                want[k].push_back(GuestLoad32(sets[k] + 4 + i * 4));
        }
    std::unique_lock l(g_m);
    std::vector<uint32_t> ready[3];
    auto scan = [&] {
        int total = 0;
        for (int k = 0; k < 3; ++k)
        {
            ready[k].clear();
            for (const uint32_t h : want[k])
            {
                const Socket* s = Find(h);
                if (s && ((k == 0 && Readable(*s)) || (k == 1 && Writable(*s)) || (k == 2 && Excepted(*s))))
                    ready[k].push_back(h);
            }
            total += int(ready[k].size());
        }
        return total;
    };
    for (int k = 0; k < 3; ++k)
        for (const uint32_t h : want[k])
            if (!Find(h))
            {
                ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
                return;
            }
    int count = scan();
    if (!count && waitMs != 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs < 0 ? 0x7FFFFFFF : waitMs);
        while (!count && g_cv.wait_until(l, deadline) != std::cv_status::timeout)
            count = scan();
        if (!count)
            count = scan();
    }
    for (int k = 0; k < 3; ++k)
        if (sets[k])
        {
            GuestStore32(sets[k], uint32_t(ready[k].size()));
            for (size_t i = 0; i < ready[k].size(); ++i)
                GuestStore32(sets[k] + 4 + uint32_t(i) * 4, ready[k][i]);
        }
    ctx.r3.u64 = uint32_t(count);
}

// __WSAFDIsSet(s, fd_set*) (no caller)
SCDA_IMPORT(NetDll___WSAFDIsSet)
{
    const uint32_t handle = ctx.r3.u32, set = ctx.r4.u32;
    uint32_t found = 0;
    if (set)
    {
        const uint32_t n = (std::min)(GuestLoad32(set), 64u);
        for (uint32_t i = 0; i < n && !found; ++i)
            found = GuestLoad32(set + 4 + i * 4) == handle;
    }
    ctx.r3.u64 = found;
}

// ioctlsocket: FIONBIO 0x8004667E (non-blocking), FIONREAD 0x4004667F (bytes waiting).
SCDA_IMPORT(NetDll_ioctlsocket)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    const uint32_t cmd = ctx.r5.u32, arg = ctx.r6.u32;
    if (cmd == 0x8004667Eu)
    {
        s->nonBlocking = arg && GuestLoad32(arg);
        ctx.r3.u64 = 0;
        return;
    }
    if (cmd == 0x4004667Fu)
    {
        const uint32_t n = s->kind == Kind::Stream ? uint32_t(s->stream.size()) : (s->datagrams.empty() ? 0 : uint32_t(s->datagrams.front().data.size()));
        if (arg)
            GuestStore32(arg, n);
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = uint32_t(Fail(WSAEOPNOTSUPP_));
}

// setsockopt(caller, s, level, name, value*, len): kept (SO_REUSEADDR 4, SO_BROADCAST 0x20 noted).
SCDA_IMPORT(NetDll_setsockopt)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    const uint32_t level = ctx.r5.u32, name = ctx.r6.u32, value = ctx.r7.u32, length = ctx.r8.u32;
    uint32_t v = 0;
    if (value && length >= 4)
        v = GuestLoad32(value);
    else if (value && length >= 1)
        v = g_base[value];
    s->options[(level << 16) ^ name] = v;
    if (level == 0xFFFF && name == 4)
        s->reuse = v != 0;
    if (level == 0xFFFF && name == 0x20)
        s->broadcast = v != 0;
    if (xnet::Trace())
        RuntimeLog("[xsocket] setsockopt %08X level %X name %X = %u\n", ctx.r4.u32, level, name, v);
    ctx.r3.u64 = 0;
}

// getsockopt: SO_TYPE 0x1008, SO_ERROR 0x1007, buffer sizes and stored values.
SCDA_IMPORT(NetDll_getsockopt)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    const uint32_t level = ctx.r5.u32, name = ctx.r6.u32, value = ctx.r7.u32, lenPtr = ctx.r8.u32;
    uint32_t v = 0;
    if (level == 0xFFFF && name == 0x1008)
        v = s->kind == Kind::Stream ? 1 : 2;
    else if (level == 0xFFFF && name == 0x1007)
    {
        v = s->pendingError;
        s->pendingError = 0;
    }
    else
    {
        const auto it = s->options.find((level << 16) ^ name);
        v = it == s->options.end() ? (level == 0xFFFF && (name == 0x1001 || name == 0x1002) ? 65536 : 0) : it->second;
    }
    if (value)
        GuestStore32(value, v);
    if (lenPtr)
        GuestStore32(lenPtr, 4);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(NetDll_getsockname)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_));
        return;
    }
    if (!s->bound)
    {
        ctx.r3.u64 = uint32_t(Fail(WSAEINVAL_));
        return;
    }
    WriteAddr(ctx.r5.u32, ctx.r6.u32, xnet::AddrOfPeer(net::LocalPeer()), s->port);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(NetDll_getpeername)
{
    OFFLINE_FAIL();
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s || !s->peer)
    {
        ctx.r3.u64 = uint32_t(Fail(s ? WSAENOTCONN_ : WSAENOTSOCK_));
        return;
    }
    WriteAddr(ctx.r5.u32, ctx.r6.u32, xnet::AddrOfPeer(s->peer), s->peerPort);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = xnet::Enabled() ? g_wsaError : WSAENETDOWN_; }
SCDA_IMPORT(NetDll_WSASetLastError) { g_wsaError = ctx.r3.u32; }

// inet_addr("a.b.c.d") (no caller): the address; "" = 0, invalid = INADDR_NONE.
SCDA_IMPORT(NetDll_inet_addr)
{
    const uint32_t text = ctx.r3.u32;
    if (!text)
    {
        ctx.r3.u64 = 0xFFFFFFFFu;
        return;
    }
    char s[32] = {};
    for (int i = 0; i < 31 && g_base[text + i]; ++i)
        s[i] = char(g_base[text + i]);
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (!s[0])
        ctx.r3.u64 = 0;
    else if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        ctx.r3.u64 = 0xFFFFFFFFu;
    else
        ctx.r3.u64 = (a << 24) | (b << 16) | (c << 8) | d;
}
