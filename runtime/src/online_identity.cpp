#include <online_identity.h>
#include <mutex>

namespace online {
namespace {
std::mutex g_mutex;
Identity g_identity;
} // namespace

Identity GetIdentity()
{
    std::lock_guard l(g_mutex);
    return g_identity;
}

void SetIdentity(const Identity& id)
{
    std::lock_guard l(g_mutex);
    g_identity = id;
}
} // namespace online
