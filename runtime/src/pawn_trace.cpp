#include <windows.h>
#include <xinput.h>
#include <runtime.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Player pawn locator (blind autopress scripts need Sam's position to steer).
// Actor layout (from camera writer sub_82C1EAC8): Location x,y,z @+148 (0x94), Rotation @+160 (0xA0).
// The listener object uses Location @+0x11C (sub_82F1F4C0) and is not laid out like an actor.
// *0x837BEDBC = audio listener: sub_82F1F4C0 copies its Location (x scale) each frame,
// so it follows the view.
//
// SCDA_FIND_PAWN=<frame>[,<frame>..][:radius]: at each frame, scan committed guest memory for
// objects whose first word is a rodata vtable (0x82000000-0x824B0000) and whose Location is
// within radius (default 800) of the listener; log them.
// SCDA_TRACE_ACTOR=<addr>[:every]: log that actor's Location/Rotation every n frames (default 30).
namespace
{
constexpr uint32_t kListenerPtr = 0x837BEDBC;
constexpr uint32_t kLisLoc = 0x11C, kLoc = 0x94, kRot = 0xA0;

float F(uint32_t a) { return std::bit_cast<float>(GuestLoad32(a)); }

bool Committed(uint32_t a, uint32_t& end)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_base + a, &mbi, sizeof(mbi)))
        return false;
    const uint64_t e = uint64_t((uint8_t*)mbi.BaseAddress - g_base) + mbi.RegionSize;
    end = e > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(e);
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

void Scan(uint64_t frame, float radius)
{
    const uint32_t lis = GuestLoad32(kListenerPtr);
    if (!lis)
    {
        RuntimeLog("[pawn] frame %llu: no listener\n", (unsigned long long)frame);
        return;
    }
    const float cx = F(lis + kLisLoc), cy = F(lis + kLisLoc + 4), cz = F(lis + kLisLoc + 8);
    RuntimeLog("[pawn] frame %llu listener %08X vt %08X at %.1f %.1f %.1f\n", (unsigned long long)frame, lis,
        GuestLoad32(lis), cx, cy, cz);
    const uint32_t g2 = GuestLoad32(0x837BEEA8);
    RuntimeLog("[pawn] *837BEEA8 = %08X vt %08X\n", g2, g2 ? GuestLoad32(g2) : 0);
    int hits = 0;
    for (uint64_t a = 0x40000000; a < 0x80000000ull && hits < 400;)
    {
        uint32_t end;
        const bool ok = Committed(uint32_t(a), end);
        if (end <= a)
            break;
        if (ok)
            for (uint32_t o = uint32_t(a); o + kRot + 12 <= end && hits < 400; o += 4)
            {
                const uint32_t vt = GuestLoad32(o);
                if (vt < 0x82000000 || vt >= 0x824B0000 || (vt & 3))
                    continue;
                const uint32_t m0 = GuestLoad32(vt), m1 = GuestLoad32(vt + 4);
                if (m0 < 0x824B0000 || m0 >= 0x83590000 || m1 < 0x824B0000 || m1 >= 0x83590000)
                    continue;
                const float x = F(o + kLoc), y = F(o + kLoc + 4), z = F(o + kLoc + 8);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || (x == 0 && y == 0 && z == 0))
                    continue;
                const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz));
                if (d > radius || o == lis)
                    continue;
                hits++;
                RuntimeLog("[pawn]  %08X vt %08X loc %.1f %.1f %.1f d %.0f rot %08X %08X %08X\n", o, vt, x, y, z, d,
                    GuestLoad32(o + kRot), GuestLoad32(o + kRot + 4), GuestLoad32(o + kRot + 8));
            }
        a = end;
    }
    // SCDA_FIND_VT=<hex>[,<hex>..]: every object with that vtable (any distance), Location @+0x94.
    // AEObjectiveLight = 82190420 (InternalConstructor sub_82B0B578).
    if (const char* r = getenv("SCDA_FIND_VT"))
    {
        std::vector<uint32_t> vts;
        for (const char* p = r; *p;)
        {
            char* e;
            vts.push_back(uint32_t(strtoul(p, &e, 16)));
            if (e == p)
                break;
            p = *e == ',' ? e + 1 : e;
        }
        int n = 0;
        for (uint64_t a = 0x40000000; a < 0x80000000ull && n < 200;)
        {
            uint32_t end;
            const bool ok = Committed(uint32_t(a), end);
            if (end <= a)
                break;
            if (ok)
                for (uint32_t o = uint32_t(a); o + kRot + 12 <= end && n < 200; o += 4)
                {
                    const uint32_t vt = GuestLoad32(o);
                    if (std::find(vts.begin(), vts.end(), vt) == vts.end())
                        continue;
                    n++;
                    const float x = F(o + kLoc), y = F(o + kLoc + 4), z = F(o + kLoc + 8);
                    RuntimeLog("[pawn] vt %08X obj %08X loc %.1f %.1f %.1f d %.0f\n", vt, o, x, y, z,
                        std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                    // AEObjectiveManager (8219B270): TArrays @+940 and @+960 (data, num, max).
                    if (vt == 0x8219B270)
                        for (uint32_t arr : {940u, 960u})
                        {
                            const uint32_t data = GuestLoad32(o + arr), num = GuestLoad32(o + arr + 4);
                            RuntimeLog("[pawn]   +%u data %08X num %u\n", arr, data, num);
                            for (uint32_t i = 0; data && i < num && i < 32; i++)
                            {
                                const uint32_t e = GuestLoad32(data + 4 * i);
                                RuntimeLog("[pawn]    [%u] %08X vt %08X loc %.1f %.1f %.1f\n", i, e,
                                    e ? GuestLoad32(e) : 0, e ? F(e + kLoc) : 0.f, e ? F(e + kLoc + 4) : 0.f,
                                    e ? F(e + kLoc + 8) : 0.f);
                            }
                        }
                }
            a = end;
        }
    }
    // SCDA_FIND_REF=<hex>[,<hex>..]: where those guest pointers are stored (image data + heap).
    if (const char* r = getenv("SCDA_FIND_REF"))
        for (const char* p = r; *p;)
        {
            char* e;
            const uint32_t want = uint32_t(strtoul(p, &e, 16));
            int refs = 0;
            for (uint64_t a = 0x40000000; a < 0x90000000ull && refs < 40;)
            {
                uint32_t end;
                const bool ok = Committed(uint32_t(a), end);
                if (end <= a)
                    break;
                if (ok)
                    for (uint32_t o = uint32_t(a); o + 4 <= end && refs < 40; o += 4)
                        if (GuestLoad32(o) == want)
                        {
                            refs++;
                            // Owner guess: nearest preceding word (within 0x1000, same region) that is a vtable.
                            uint32_t owner = 0;
                            for (uint32_t b = o & ~3u; b >= uint32_t(a) && o - b < 0x1000; b -= 4)
                            {
                                const uint32_t vt = GuestLoad32(b);
                                if (vt >= 0x82000000 && vt < 0x824B0000 && !(vt & 3))
                                {
                                    const uint32_t m0 = GuestLoad32(vt), m1 = GuestLoad32(vt + 4);
                                    if (m0 >= 0x824B0000 && m0 < 0x83590000 && m1 >= 0x824B0000 && m1 < 0x83590000)
                                    {
                                        owner = b;
                                        break;
                                    }
                                }
                                if (b < 4)
                                    break;
                            }
                            RuntimeLog("[pawn] ref %08X at %08X owner %08X vt %08X +%X\n", want, o, owner,
                                owner ? GuestLoad32(owner) : 0, owner ? o - owner : 0);
                        }
                a = end;
            }
            p = *e == ',' ? e + 1 : e;
            if (e == p && *p != ',')
                break;
        }
    RuntimeLog("[pawn] frame %llu: %d candidates\n", (unsigned long long)frame, hits);
}
} // namespace

namespace { uint32_t FindCamera(float px, float py); }

// Called once per input poll with the current frame number.
void PawnTraceTick(uint64_t frame)
{
    static const auto cfg = []
    {
        struct { std::vector<uint64_t> frames; float radius = 600; uint32_t actor = 0, every = 30; } c;
        if (const char* s = getenv("SCDA_FIND_PAWN"))
        {
            const char* colon = strchr(s, ':');
            for (const char* p = s; *p && p != colon;)
            {
                c.frames.push_back(strtoull(p, (char**)&p, 10));
                if (*p == ',')
                    p++;
                else
                    break;
            }
            if (colon)
                c.radius = float(atof(colon + 1));
        }
        if (const char* s = getenv("SCDA_TRACE_ACTOR"))
        {
            char* e;
            c.actor = uint32_t(strtoul(s, &e, 16));
            if (*e == ':')
                c.every = uint32_t(strtoul(e + 1, nullptr, 10));
            if (!c.every)
                c.every = 30;
        }
        return c;
    }();
    // SCDA_TRACE_SPEED=1|<ms>: every second (or <ms>), frames presented, the game clock (0x401AAB48, raw and
    // as a float), the listener Location and the nearest camera's yaw: game speed / look rate at
    // different frame rates (60 fps check). The camera is searched again every 2 s.
    static const char* speedSpec = getenv("SCDA_TRACE_SPEED");
    static const ULONGLONG speedMs = speedSpec ? std::max<ULONGLONG>(atoi(speedSpec) > 1 ? atoi(speedSpec) : 1000, 10) : 0;
    static ULONGLONG nextSpeed = 0, nextCamera = 0;
    static uint32_t camera = 0;
    if (speedMs && GetTickCount64() >= nextSpeed)
    {
        nextSpeed = GetTickCount64() + speedMs;
        const uint32_t lis = GuestLoad32(kListenerPtr), clock = GuestLoad32(0x401AAB48);
        const float lx = lis ? F(lis + kLisLoc) : 0.f, ly = lis ? F(lis + kLisLoc + 4) : 0.f;
        if (lis && GetTickCount64() >= nextCamera)
        {
            nextCamera = GetTickCount64() + 2000;
            camera = FindCamera(lx, ly);
        }
        RuntimeLog("[speed] t %.3f frame %llu clock %08X %.4f lis %.1f %.1f %.1f yaw %d pitch %d\n", double(GetTickCount64()) / 1000.0,
            (unsigned long long)frame, clock, double(std::bit_cast<float>(clock)), lx, ly, lis ? F(lis + kLisLoc + 8) : 0.f,
            camera ? int32_t(GuestLoad32(camera + 0xA4)) : 0, camera ? int32_t(GuestLoad32(camera + 0xA0)) : 0);
    }
    static uint64_t last = ~0ull;
    if (frame == last)
        return;
    last = frame;
    for (uint64_t f : cfg.frames)
        if (f == frame)
            Scan(frame, cfg.radius);
    if (cfg.actor && frame % cfg.every == 0)
    {
        const uint32_t a = cfg.actor;
        RuntimeLog("[actor] frame %llu %08X loc %.1f %.1f %.1f rot %08X %08X %08X\n", (unsigned long long)frame, a,
            F(a + kLoc), F(a + kLoc + 4), F(a + kLoc + 8), GuestLoad32(a + kRot), GuestLoad32(a + kRot + 4),
            GuestLoad32(a + kRot + 8));
    }
}


// SCDA_AUTOPILOT=<start frame>:<x>,<y>[,<r>];<x>,<y>[,<r>];...  (or @<file>, newlines = ';')
// From the start frame, drive the left stick toward each waypoint in turn (default reach radius 80).
// Sam = listener Location; stick input is camera-relative, so the world heading is rotated by the
// yaw of the nearest camera actor (vtable kCameraVt, int FRotator yaw @+0xA4, 65536 = 360 deg).
// Stuck (< 15 units per 45 frames): strafe 40 frames, alternating sides.
namespace
{
constexpr uint32_t kCameraVt = 0x821518E8;
struct Waypoint { float x, y, r; };

uint32_t FindCamera(float px, float py)
{
    uint32_t best = 0;
    float bestD = 1e30f;
    for (uint64_t a = 0x40000000; a < 0x80000000ull;)
    {
        uint32_t end;
        const bool ok = Committed(uint32_t(a), end);
        if (end <= a)
            break;
        if (ok)
            for (uint32_t o = uint32_t(a); o + kRot + 12 <= end; o += 16)
                if (GuestLoad32(o) == kCameraVt)
                {
                    const float dx = F(o + kLoc) - px, dy = F(o + kLoc + 4) - py, d = dx * dx + dy * dy;
                    if (d < bestD)
                        bestD = d, best = o;
                }
        a = end;
    }
    return bestD < 1500.f * 1500.f ? best : 0;
}
} // namespace

void AutopilotApply(uint64_t frame, XINPUT_GAMEPAD& g)
{
    static const auto plan = []
    {
        struct { uint64_t start = 0; std::vector<Waypoint> pts; } p;
        const char* env = getenv("SCDA_AUTOPILOT");
        if (!env)
            return p;
        std::string text = env;
        if (env[0] == '@')
        {
            text.clear();
            if (FILE* f = fopen(env + 1, "rb"))
            {
                char buf[4096];
                for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
                    text.append(buf, n);
                fclose(f);
            }
            for (auto& c : text)
                if (c == '\n' || c == '\r')
                    c = ';';
        }
        const char* s = text.c_str();
        p.start = strtoull(s, (char**)&s, 10);
        if (*s == ':')
            s++;
        while (*s)
        {
            Waypoint w{0, 0, 80};
            char* e;
            w.x = strtof(s, &e);
            if (e == s) { s++; continue; }
            s = e + (*e == ',');
            w.y = strtof(s, &e);
            s = e;
            if (*s == ',')
                w.r = strtof(s + 1, (char**)&s);
            p.pts.push_back(w);
        }
        RuntimeLog("[autopilot] start %llu, %zu waypoints\n", (unsigned long long)p.start, p.pts.size());
        return p;
    }();
    if (plan.pts.empty() || frame < plan.start)
        return;
    static size_t next;
    static uint32_t cam;
    static uint64_t camCheck, lastMoveFrame, strafeUntil, logged;
    static float lastX, lastY;
    static int side = 1;
    if (next >= plan.pts.size())
        return;
    const uint32_t lis = GuestLoad32(kListenerPtr);
    if (!lis)
        return;
    const float x = F(lis + kLisLoc), y = F(lis + kLisLoc + 4);
    if ((!cam || GuestLoad32(cam) != kCameraVt) && frame >= camCheck)
    {
        cam = FindCamera(x, y);
        camCheck = frame + 150;
        RuntimeLog("[autopilot] frame %llu camera %08X\n", (unsigned long long)frame, cam);
    }
    if (!cam)
        return;
    const Waypoint& w = plan.pts[next];
    const float dx = w.x - x, dy = w.y - y;
    if (dx * dx + dy * dy < w.r * w.r)
    {
        RuntimeLog("[autopilot] frame %llu reached #%zu (%.0f %.0f)\n", (unsigned long long)frame, next, w.x, w.y);
        next++;
        lastMoveFrame = frame;
        return;
    }
    if (lastMoveFrame == 0 || (x - lastX) * (x - lastX) + (y - lastY) * (y - lastY) > 15.f * 15.f)
        lastX = x, lastY = y, lastMoveFrame = frame;
    else if (frame - lastMoveFrame > 45 && frame >= strafeUntil)
    {
        strafeUntil = frame + 40;
        side = -side;
        lastMoveFrame = frame;
        RuntimeLog("[autopilot] frame %llu stuck at %.0f %.0f, strafe %d\n", (unsigned long long)frame, x, y, side);
    }
    const float camYaw = float(int16_t(GuestLoad32(cam + kRot + 4) & 0xFFFF)) * (6.2831853f / 65536.f);
    float rel = std::atan2(dy, dx) - camYaw;
    if (frame < strafeUntil)
        rel += side * 1.5707963f;
    // UE yaw turns clockwise seen from above (X forward, Y right): positive rel = goal to the right.
    g.sThumbLX = int16_t(std::sin(rel) * 32767.f);
    g.sThumbLY = int16_t(std::cos(rel) * 32767.f);
    if (frame >= logged + 60)
    {
        logged = frame;
        RuntimeLog("[autopilot] frame %llu t %.2f pos %.0f %.0f -> #%zu (%.0f %.0f) d %.0f camYaw %.0f rel %.0f\n",
            (unsigned long long)frame, double(GetTickCount64()) / 1000.0, x, y, next, w.x, w.y, std::sqrt(dx * dx + dy * dy), camYaw * 57.29578f,
            std::remainder(rel, 6.2831853f) * 57.29578f);
    }
}

// SCDA_PEEK=<frame>: (first input poll at or after it) <hex addr>,<bytes>[;<hex addr>,<bytes>..]: at that frame, byte stats of each guest range.
void PeekTick(uint64_t frame)
{
    static const char* spec = getenv("SCDA_PEEK");
    static bool done;
    if (!spec || done || frame < strtoull(spec, nullptr, 10))
        return;
    done = true;
    for (const char* p = strchr(spec, ':'); p && *p;)
    {
        char* e;
        const uint32_t a = uint32_t(strtoul(p + 1, &e, 16));
        const uint32_t n = *e == ',' ? uint32_t(strtoul(e + 1, &e, 10)) : 0;
        uint64_t sum = 0;
        uint32_t lo = 255, hi = 0, hist[8] = {};
        for (uint32_t i = 0; i < n; i++)
        {
            const uint8_t v = g_base[a + i];
            sum += v, lo = v < lo ? v : lo, hi = v > hi ? v : hi, hist[v >> 5]++;
        }
        RuntimeLog("[peek] frame %llu %08X+%u avg %.1f min %u max %u hist %u %u %u %u %u %u %u %u\n",
            (unsigned long long)frame, a, n, n ? double(sum) / n : 0.0, lo, hi, hist[0], hist[1], hist[2], hist[3],
            hist[4], hist[5], hist[6], hist[7]);
        p = strchr(e, ';');
    }
}

