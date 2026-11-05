#include <online_identity.h>
#include <mutex>
#include <settings.h>

namespace online {
namespace {
std::mutex g_mutex;
Identity g_identity;
} // namespace

Identity GetIdentity()
{
    std::lock_guard l(g_mutex);
    // The local profile name ([Profile] Name), read once at the first XamUser* call.
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (!g_identity.live)
            if (std::string n = ProfileNameSetting(); !n.empty())
                g_identity.gamertag = n;
    }
    return g_identity;
}

void SetIdentity(const Identity& id)
{
    std::lock_guard l(g_mutex);
    g_identity = id;
}
} // namespace online
