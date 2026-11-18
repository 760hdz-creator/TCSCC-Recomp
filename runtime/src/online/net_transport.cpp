// Packet transport (net_transport.h). The loopback backend: one UDP socket per instance on
// 127.0.0.1:28200+n (the first free n of 64); peer id = 0x4C4F4F50'0000 | port. Localhost UDP does not
// reorder; a large receive buffer keeps it lossless in practice (Steam's reliable channel replaces it
// for real play). Tests can add delay, jitter and loss (SCDA_NET_DELAY / _JITTER / _LOSS).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include "net_transport.h"
#include "steam_net.h"
#include <runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace net {
namespace {

constexpr uint64_t kLoopTag = 0x4C4F4F5000000000ull; // "LOOP"
constexpr uint16_t kBasePort = 28200;

std::mutex g_mutex;
std::atomic<bool> g_started{ false };
std::atomic<uint64_t> g_local{ 0 };
SOCKET g_socket = INVALID_SOCKET;
Receiver g_receiver = nullptr;
bool g_steam = false;

void FromSteam(uint64_t peer, bool reliable, const uint8_t* data, size_t size)
{
    if (g_receiver)
        g_receiver(peer, reliable ? Channel::Reliable : Channel::Unreliable, data, size);
}

// Network conditions for tests (loopback only). Packets wait in a queue and leave from a sender thread
// at their time; the reliable channel keeps its order and its "lost" packets come one re-send later
// (it stands in for Steam's reliable messages).
struct Conditions { int delayMs = 0, jitterMs = 0, lossPct = 0; };
const Conditions& Cond()
{
    static const Conditions c = [] {
        auto get = [](const char* n, int hi) { const char* e = std::getenv(n); return e ? (std::max)(0, (std::min)(std::atoi(e), hi)) : 0; };
        Conditions r{ get("SCDA_NET_DELAY", 2000), get("SCDA_NET_JITTER", 2000), get("SCDA_NET_LOSS", 100) };
        if (r.delayMs || r.jitterMs || r.lossPct)
            RuntimeLog("[net] test conditions: %d ms one way + 0..%d ms jitter, %d%% loss (reliable: re-sent)\n", r.delayMs, r.jitterMs, r.lossPct);
        return r;
    }();
    return c;
}
struct Delayed
{
    std::chrono::steady_clock::time_point due;
    uint64_t order;
    sockaddr_in to;
    std::vector<uint8_t> bytes;
    bool operator>(const Delayed& o) const { return due != o.due ? due > o.due : order > o.order; }
};
std::mutex g_delayMutex;
std::condition_variable g_delayCv;
std::priority_queue<Delayed, std::vector<Delayed>, std::greater<Delayed>> g_delayed;
uint64_t g_delayOrder = 0;
std::chrono::steady_clock::time_point g_lastReliableDue{};
uint64_t g_rand = 0;
uint32_t NextRandom()
{
    if (!g_rand)
    {
        const char* e = std::getenv("SCDA_NET_SEED");
        g_rand = (e ? std::strtoull(e, nullptr, 10) : 0) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    }
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 7;
    g_rand ^= g_rand << 17;
    return uint32_t(g_rand >> 16);
}
void SendLoop()
{
    std::unique_lock l(g_delayMutex);
    for (;;)
    {
        if (g_delayed.empty())
        {
            g_delayCv.wait(l);
            continue;
        }
        const auto due = g_delayed.top().due;
        if (std::chrono::steady_clock::now() < due)
        {
            g_delayCv.wait_until(l, due);
            continue;
        }
        Delayed d = g_delayed.top();
        g_delayed.pop();
        l.unlock();
        sendto(g_socket, reinterpret_cast<const char*>(d.bytes.data()), int(d.bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&d.to), sizeof(d.to));
        l.lock();
    }
}
bool SendDelayed(const sockaddr_in& to, bool reliable, std::vector<uint8_t> bytes)
{
    const Conditions& c = Cond();
    std::lock_guard l(g_delayMutex);
    const bool lost = c.lossPct && int(NextRandom() % 100) < c.lossPct;
    if (!reliable && lost)
        return true; // lost on the way
    auto due = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(c.delayMs + (c.jitterMs ? int(NextRandom() % uint32_t(c.jitterMs + 1)) : 0));
    if (reliable)
    {
        // A lost reliable packet arrives one re-send later (Steam: about one round trip), and holds back
        // the ones behind it (in order).
        if (lost)
            due += std::chrono::milliseconds(2 * c.delayMs + 20);
        if (due < g_lastReliableDue)
            due = g_lastReliableDue;
        g_lastReliableDue = due;
    }
    static bool started = false;
    if (!started)
    {
        started = true;
        std::thread(SendLoop).detach();
    }
    g_delayed.push({ due, g_delayOrder++, to, std::move(bytes) });
    g_delayCv.notify_one();
    return true;
}

void ReceiveLoop()
{
    std::vector<uint8_t> buffer(65536);
    for (;;)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(g_socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 1)
        {
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK)
                return;
            continue;
        }
        // First byte: the channel; the rest is the packet.
        if (g_receiver)
            g_receiver(kLoopTag | ntohs(from.sin_port), Channel(buffer[0] & 1), buffer.data() + 1, size_t(n - 1));
    }
}

} // namespace

// Includes the build version: co-op runs the same simulation on both PCs, and builds that differ in
// anything the simulation sees (0.1.8 had no 30 fps hold) desync within a few steps (forge #3).
// Lobby search and Steam invites match this string exactly, so other builds never see the session.
const char* GameProtocol() { return "tcscc-recomp-1-" SCDA_VERSION; }

bool IsLoopbackPeer(uint64_t peer) { return (peer & 0xFFFFFFFF00000000ull) == kLoopTag; }
uint64_t LoopbackPeer(int index) { return kLoopTag | uint16_t(kBasePort + index); }

bool Start(Receiver receiver)
{
    std::lock_guard l(g_mutex);
    if (g_started)
        return true;
    const char* choice = std::getenv("SCDA_NET");
    const bool wantLoopback = choice && std::strcmp(choice, "loopback") == 0;
    const bool needSteam = choice && std::strcmp(choice, "steam") == 0;
    if (!wantLoopback)
    {
        g_receiver = receiver;
        if (steamnet::Start(FromSteam))
        {
            g_steam = true;
            g_local = steamnet::LocalId();
            g_started = true;
            RuntimeLog("[net] Steam transport (peer %016llX)\n", (unsigned long long)g_local.load());
            return true;
        }
        if (needSteam)
            return false;
        RuntimeLog("[net] Steam unavailable - loopback transport (this PC only)\n");
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return false;
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
        return false;
    int big = 8 << 20;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    setsockopt(g_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    BOOL exclusive = TRUE;
    setsockopt(g_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    // ICMP "port unreachable" must not turn into errors on the receive loop.
    BOOL off = FALSE;
    DWORD ignored = 0;
    WSAIoctl(g_socket, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
    uint16_t port = 0;
    for (uint16_t n = 0; n < 64 && !port; ++n)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(kBasePort + n));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0)
            port = uint16_t(kBasePort + n);
    }
    if (!port)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return false;
    }
    g_receiver = receiver;
    g_local = kLoopTag | port;
    g_started = true;
    std::thread(ReceiveLoop).detach();
    RuntimeLog("[net] loopback transport on 127.0.0.1:%u (peer %016llX)\n", port, (unsigned long long)g_local.load());
    return true;
}

uint64_t LocalPeer() { return g_local.load(); }
const char* BackendName() { return !g_started ? "none" : g_steam ? "steam" : "loopback"; }

bool Send(uint64_t peer, Channel channel, const void* data, size_t size)
{
    if (g_steam)
        return steamnet::Send(peer, channel == Channel::Reliable, data, size);
    if (!g_started || !IsLoopbackPeer(peer) || size + 1 > 65507)
        return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(peer & 0xFFFF));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    thread_local std::vector<uint8_t> packet;
    packet.resize(size + 1);
    packet[0] = uint8_t(channel);
    if (size)
        std::memcpy(packet.data() + 1, data, size);
    const Conditions& c = Cond();
    if (c.delayMs || c.jitterMs || c.lossPct)
        return SendDelayed(to, channel == Channel::Reliable, packet);
    return sendto(g_socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
                  reinterpret_cast<sockaddr*>(&to), sizeof(to)) == int(packet.size());
}

} // namespace net
