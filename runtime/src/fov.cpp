// Field of view override (F1 menu): the player controller's FOV properties, found through UE2
// reflection on the controller object seen by the script-event thunk (travel_trace.cpp hook of
// sub_8258E908). Layout (STATE.md): UObject +0x20 name, +0x24 class; UField +0x2C super, +0x30 next;
// UStruct +0x40 children; UProperty +0x48 offset (high 16 bits).
#include <runtime.h>
#include <fov.h>
#include "gpu/draw_state.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

// GNames = TArray<FNameEntry*> at 0x82F284D4; entry +12 = name (ANSI, or UTF-16 BE when the first byte is 0).
std::string GuestFNameOf(uint32_t index)
{
    const uint32_t data = GuestLoad32(0x82F284D4);
    const uint32_t num = GuestLoad32(0x82F284D8);
    if (!data || index >= num)
        return "?";
    const uint32_t entry = GuestLoad32(data + index * 4);
    if (!entry)
        return "?";
    std::string s;
    const bool wide = *(g_base + entry + 12) == 0;
    for (uint32_t i = 0; i < 64; i++)
    {
        const uint32_t c = wide ? GuestLoad16(entry + 12 + i * 2) : *(g_base + entry + 12 + i);
        if (!c)
            break;
        s += c < 0x80 ? char(c) : '?';
    }
    return s;
}

namespace {

std::string NameOf(uint32_t obj) { return obj ? GuestFNameOf(GuestLoad32(obj + 0x20)) : ""; }
std::string ClassName(uint32_t obj) { return obj ? NameOf(GuestLoad32(obj + 0x24)) : ""; }

// Byte offset of a property by name in a class or its supers, 0 when absent.
uint32_t PropertyOffset(uint32_t cls, const char* name)
{
    for (uint32_t depth = 0; cls && depth < 16; cls = GuestLoad32(cls + 0x2C), depth++)
        for (uint32_t f = GuestLoad32(cls + 0x40), n = 0; f && n < 4000; f = GuestLoad32(f + 0x30), n++)
            if (NameOf(f) == name)
                return GuestLoad32(f + 0x48) >> 16;
    return 0;
}

uint32_t g_controller, g_controllerClass;
float g_gameDefault = 0; // the controller's DefaultFOV before any override (75)
std::atomic<float> g_userFov{ 0.0f };
std::unordered_map<uint32_t, bool> g_classIsController; // class object -> name contains PlayerController
struct Offsets
{
    uint32_t samCamera = 0, desired = 0, def = 0, angle = 0;
} g_off;

float LoadFloat(uint32_t a)
{
    const uint32_t bits = GuestLoad32(a);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}
void StoreFloat(uint32_t a, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    GuestStore32(a, bits);
}

} // namespace

void SetFovOverride(float degrees) { g_userFov = degrees; }

void FovReset()
{
    g_controller = g_controllerClass = 0;
    g_gameDefault = 0;
    g_classIsController.clear();
    g_off = {};
}

void FovSeeObject(uint32_t obj)
{
    if (!obj)
        return;
    const uint32_t cls = GuestLoad32(obj + 0x24);
    if (!cls)
        return;
    if (cls == g_controllerClass)
    {
        g_controller = obj;
        return;
    }
    auto it = g_classIsController.find(cls);
    if (it == g_classIsController.end())
        it = g_classIsController.emplace(cls, NameOf(cls).find("PlayerController") != std::string::npos).first;
    if (!it->second)
        return;
    g_controller = obj;
    g_controllerClass = cls;
    g_off.samCamera = PropertyOffset(cls, "m_samCameraFOV");
    g_off.desired = PropertyOffset(cls, "DesiredFOV");
    g_off.def = PropertyOffset(cls, "DefaultFOV");
    g_off.angle = PropertyOffset(cls, "FovAngle");
    if (g_off.def && !g_gameDefault)
        g_gameDefault = LoadFloat(obj + g_off.def);
    RuntimeLog("[fov] controller %08X %s: m_samCameraFOV +%X %g, DesiredFOV +%X %g, DefaultFOV +%X %g, FovAngle +%X %g\n", obj,
        NameOf(cls).c_str(), g_off.samCamera, LoadFloat(obj + g_off.samCamera), g_off.desired, LoadFloat(obj + g_off.desired), g_off.def,
        LoadFloat(obj + g_off.def), g_off.angle, LoadFloat(obj + g_off.angle));
}

// Engine tick (travel_trace.cpp sub_825C6C68 hook). The view follows FovAngle, which the scripts ease
// toward DesiredFOV; zooms set DesiredFOV and ResetFOV restores DefaultFOV. So: DefaultFOV = the
// user's value, and DesiredFOV too while it sits at a default (not zoomed). The game's own value
// (75) writes nothing.
// SCDA_FOV_TEST=<sam|desired|angle>:<degrees>: write that property every tick instead (diagnostic).

void FovTick()
{
    // Reflection dumps, once a controller exists (finding script properties without the packages):
    // SCDA_DUMP_NAMES=<sub>[,<sub>..] logs every GNames entry containing a substring (case-insensitive);
    // SCDA_DUMP_PROPS=<class>[,<class>..] logs each class's own properties + byte offsets, found
    // through GObjects (TArray<UObject*> at 0x82E1F994).
    static const char* dumpNames = getenv("SCDA_DUMP_NAMES");
    static const char* dumpProps = getenv("SCDA_DUMP_PROPS");
    static bool dumped = false;
    if ((dumpNames || dumpProps) && !dumped && g_controller)
    {
        dumped = true;
        auto lower = [](std::string s) {
            for (char& c : s)
                c = char(tolower(uint8_t(c)));
            return s;
        };
        auto matches = [](const std::string& list, const std::string& s, bool substring) {
            for (size_t p = 0; p < list.size();)
            {
                const size_t e = list.find(',', p);
                const std::string item = list.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (!item.empty() && (substring ? s.find(item) != std::string::npos : s == item))
                    return true;
                p = e == std::string::npos ? list.size() : e + 1;
            }
            return false;
        };
        if (dumpNames)
            for (uint32_t i = 0, n = GuestLoad32(0x82F284D8); i < n; i++)
                if (const std::string name = GuestFNameOf(i); matches(lower(dumpNames), lower(name), true))
                    RuntimeLog("[names] %u %s\n", i, name.c_str());
        const uint32_t data = GuestLoad32(0x82E1F994), num = GuestLoad32(0x82E1F998);
        if (dumpProps && data && num < 4000000 && GuestRangeReadable(g_base + data, size_t(num) * 4))
            for (uint32_t i = 0; i < num; i++)
            {
                const uint32_t o = GuestLoad32(data + i * 4);
                if (!o || !GuestRangeReadable(g_base + o, 0x50) || ClassName(o) != "Class" || !matches(dumpProps, NameOf(o), false))
                    continue;
                for (uint32_t f = GuestLoad32(o + 0x40), n = 0; f && n < 4000; f = GuestLoad32(f + 0x30), n++)
                    RuntimeLog("[props] %s.%s %s +%X\n", NameOf(o).c_str(), NameOf(f).c_str(), ClassName(f).c_str(), GuestLoad32(f + 0x48) >> 16);
            }
    }
    if (!g_controller)
        return;
    static const char* test = getenv("SCDA_FOV_TEST");
    if (!test)
    {
        const float user = g_userFov;
        if (!g_off.def || !g_off.desired || !g_gameDefault || user < 1.0f)
            return;
        const float def = LoadFloat(g_controller + g_off.def), desired = LoadFloat(g_controller + g_off.desired);
        if (user == g_gameDefault && def == g_gameDefault)
            return;
        if (desired == def || desired == g_gameDefault)
            StoreFloat(g_controller + g_off.desired, user);
        StoreFloat(g_controller + g_off.def, user);
        return;
    }
    const char* colon = strchr(test, ':');
    const float v = colon ? float(atof(colon + 1)) : 90.0f;
    const std::string which(test, colon ? size_t(colon - test) : strlen(test));
    if (which == "sam" && g_off.samCamera)
        StoreFloat(g_controller + g_off.samCamera, v);
    else if (which == "desired" && g_off.desired && g_off.def)
    {
        StoreFloat(g_controller + g_off.desired, v);
        StoreFloat(g_controller + g_off.def, v);
    }
    else if (which == "angle" && g_off.angle)
        StoreFloat(g_controller + g_off.angle, v);
}
