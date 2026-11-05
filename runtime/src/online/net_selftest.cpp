// SCDA_NET_SELFTEST=1: the transport without the game's network code (online step 1 check).
//  - loopback (SCDA_NET=loopback): every second, "ping" to instances 0..3 except this one; replies "pong".
//    Two instances on one PC log each other's pings and pongs.
//  - Steam: a test lobby is published, then the lobby list is searched until it shows (our own lobby
//    counts), and the friends list is read.
#include "net_transport.h"
#include "steam_net.h"
#include <runtime.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
std::atomic<int> g_pings{ 0 }, g_pongs{ 0 };

void Receive(uint64_t from, net::Channel channel, const uint8_t* data, size_t size)
{
    const bool ping = size == 4 && std::memcmp(data, "ping", 4) == 0;
    const bool pong = size == 4 && std::memcmp(data, "pong", 4) == 0;
    if (ping)
    {
        if (g_pings++ < 5)
            RuntimeLog("[net-test] ping from %016llX (%s)\n", (unsigned long long)from, channel == net::Channel::Reliable ? "reliable" : "unreliable");
        net::Send(from, net::Channel::Reliable, "pong", 4);
    }
    else if (pong && g_pongs++ < 5)
        RuntimeLog("[net-test] pong from %016llX\n", (unsigned long long)from);
}
} // namespace

void NetSelfTestStart()
{
    if (!getenv("SCDA_NET_SELFTEST"))
        return;
    std::thread([] {
        if (!net::Start(Receive))
        {
            RuntimeLog("[net-test] transport did not start\n");
            return;
        }
        const uint64_t self = net::LocalPeer();
        RuntimeLog("[net-test] backend %s, local peer %016llX\n", net::BackendName(), (unsigned long long)self);
        if (!std::strcmp(net::BackendName(), "steam"))
        {
            std::vector<uint8_t> record = { 'S', 'C', 'C', '1' };
            for (int i = 0; i < 8; i++)
                record.push_back(uint8_t(self >> (8 * i)));
            steamnet::PublishLobby(self, record);
            for (int attempt = 0; attempt < 10; attempt++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                const auto lobbies = steamnet::ListLobbies(50, 5000);
                bool own = false;
                for (const auto& r : lobbies)
                    own |= r == record;
                RuntimeLog("[net-test] lobby search: %zu session(s)%s\n", lobbies.size(), own ? ", our own among them" : "");
                if (own)
                    break;
            }
            const auto friends = steamnet::Friends(5000);
            RuntimeLog("[net-test] %zu online friend(s)\n", friends.size());
            steamnet::RemoveLobby(self);
            return;
        }
        for (int second = 0; second < 600; second++)
        {
            for (int n = 0; n < 4; n++)
                if (net::LoopbackPeer(n) != self)
                    net::Send(net::LoopbackPeer(n), net::Channel::Unreliable, "ping", 4);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}
