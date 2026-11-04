// Camera / field of view (Conviction). sub_82E32910 (view frustum: atan(tan(FOV/2) * SizeY / SizeX))
// reads the horizontal FOV in degrees at view+908, view = sub_82CF6DA0() (getter).
#include <runtime.h>
#include <windows.h>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include "camera.h"


// SCDA_TRACE_CAMERA=1: once per second, the getter's result, its vtable and FOV (+908), with the caller.
extern "C" PPC_FUNC(__imp__sub_82CF6DA0);
PPC_FUNC(sub_82CF6DA0)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82CF6DA0(ctx, base);
    static const bool trace = getenv("SCDA_TRACE_CAMERA") != nullptr;
    static ULONGLONG next = 0;
    if (trace && GetTickCount64() >= next)
    {
        next = GetTickCount64() + 1000;
        const uint32_t v = ctx.r3.u32;
        RuntimeLog("[camera] view %08X vt %08X fov %g lr %08X\n", v, v ? PPC_LOAD_U32(v) : 0,
            v ? double(std::bit_cast<float>(PPC_LOAD_U32(v + 908))) : 0.0, lr);
    }
}

// SCDA_TRACE_CAMERA=1: tan(x) calls (sub_82F39AF0) per call site, once per second each: the argument
// (FOV * pi/360 for a 90 degree FOV = 0.785) and the caller's r29/r31 (callee-saved: the caller's view).
#include <mutex>
#include <unordered_map>
extern "C" PPC_FUNC(__imp__sub_82F39AF0);
PPC_FUNC(sub_82F39AF0)
{
    static const bool trace = getenv("SCDA_TRACE_CAMERA") != nullptr;
    if (trace)
    {
        static std::mutex m;
        static std::unordered_map<uint32_t, ULONGLONG> next;
        std::lock_guard lock(m);
        const uint32_t lr = uint32_t(ctx.lr);
        ULONGLONG& n = next[lr];
        if (GetTickCount64() >= n && next.size() < 64)
        {
            n = GetTickCount64() + 1000;
            RuntimeLog("[tan] lr %08X x %g r29 %08X r31 %08X r28 %08X\n", lr, ctx.f1.f64, ctx.r29.u32, ctx.r31.u32, ctx.r28.u32);
        }
    }
    __imp__sub_82F39AF0(ctx, base);
}

// Perspective builder sub_82903548(params, flags): params +0 FOV (deg, horizontal), +4/+8 near/far,
// +12/+16 width/height; P00 = cot(FOV/2), P11 = cot(FOV/2) * width/height; matrix out at params+52.
extern "C" PPC_FUNC(__imp__sub_82903548);
PPC_FUNC(sub_82903548)
{
    static const bool trace = getenv("SCDA_TRACE_CAMERA") != nullptr;
    const uint32_t p = ctx.r3.u32;
    if (trace)
    {
        static std::mutex m;
        static std::unordered_map<uint32_t, ULONGLONG> next;
        std::lock_guard lock(m);
        const uint32_t lr = uint32_t(ctx.lr);
        ULONGLONG& n = next[lr];
        if (GetTickCount64() >= n && next.size() < 64)
        {
            n = GetTickCount64() + 1000;
            auto F = [&](uint32_t o) { return double(std::bit_cast<float>(PPC_LOAD_U32(p + o))); };
            RuntimeLog("[proj] lr %08X params %08X flags %X fov %g near %g far %g w %g h %g\n", lr, p, ctx.r4.u32, F(0), F(4), F(8), F(12), F(16));
        }
    }
    // Hor+ (ultrawide): the guest still renders 1040x576, shown stretched to the window aspect A; with
    // k = A / (16/9) the FOV widens to 2 atan(k tan(FOV/2)) and the width grows by k, so P00 = cot/k and
    // P11 is unchanged (same vertical view). Inputs restored after the call (the struct persists).
    const float k = CameraHorPlusFactor();
    const uint32_t fov = PPC_LOAD_U32(p), width = PPC_LOAD_U32(p + 12);
    if (k > 1.0f)
    {
        const float f = std::bit_cast<float>(fov);
        if (f > 0.1f && f < 3.0f)
        {
            PPC_STORE_U32(p, std::bit_cast<uint32_t>(2.0f * std::atan(k * std::tan(f * 0.5f))));
            PPC_STORE_U32(p + 12, std::bit_cast<uint32_t>(std::bit_cast<float>(width) * k));
        }
    }
    __imp__sub_82903548(ctx, base);
    if (k > 1.0f)
    {
        PPC_STORE_U32(p, fov);
        PPC_STORE_U32(p + 12, width);
    }
}
