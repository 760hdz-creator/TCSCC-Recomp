#include <runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Conviction l3d renderer traces. R = renderer (ctor sub_82EF3CA8), S = *(R+23832) scene view.
// Pass gates: (S+324 & 27) != 0 skips both passes below; byte R+24456 (= S+432, set by
// sub_82D8BF68 from the "Gadget_SonarGoogle" handlers sub_82BC7000/sub_82BC6BF0) picks the
// sonar pass sub_82EFD598 (byte 1) or the lighting pass sub_82F0F078 (byte 0).
//
// Lighting pass sub_82F0F078(P) draws bucket i with sub_82F0F018(P, i, name): bucket i =
// { u32 items @P+16+12i, s32 count @P+20+12i }; names "Opaque" (0), "OpaqueEmissive" (1), ...
// SCDA_TRACE_L3D=<n>: log every bucket count for the first n lighting passes.
void WatchAdd(uint32_t addr, int maxHits);
extern "C" PPC_FUNC(__imp__sub_82F0F078);

PPC_FUNC(sub_82F0F078)
{
    static const long limit = getenv("SCDA_TRACE_L3D") ? atol(getenv("SCDA_TRACE_L3D")) : 0;
    static long calls = 0;
    const uint32_t p = ctx.r3.u32;
    // SCDA_WATCH_L3D=1: hardware write watch on the visible-list count (who fills it).
    static bool armed;
    if (!armed && getenv("SCDA_WATCH_L3D") && atoi(getenv("SCDA_WATCH_L3D")) == 1)
    {
        armed = true;
        // Bucket source (sub_82F0E258): visible list of view R[24092], R = *P, entry at
        // *(R+16) + 12*view: { u32 items @+152, s32 count @+156 }.
        const uint32_t r = PPC_LOAD_U32(p), view = PPC_LOAD_U32(r + 24092);
        const uint32_t list = PPC_LOAD_U32(r + 16) + 12 * view;
        RuntimeLog("[l3d] view %u visible list count %d @0x%08X\n", view, int32_t(PPC_LOAD_U32(list + 156)), list + 156);
        WatchAdd(list + 156, 30);
    }
    __imp__sub_82F0F078(ctx, base);
    if (calls < limit)
    {
        char line[256];
        int len = snprintf(line, sizeof(line), "[l3d] lighting %ld P 0x%08X buckets:", calls, p);
        for (uint32_t i = 0; i < 12; i++)
            len += snprintf(line + len, sizeof(line) - len, " %d", int32_t(PPC_LOAD_U32(p + 20 + 12 * i)));
        RuntimeLog("%s\n", line);
        calls++;
    }
}

// sub_82F0F018(P, i, name): one bucket draw. Logs bucket index, count and name (first n calls).
extern "C" PPC_FUNC(__imp__sub_82F0F018);

PPC_FUNC(sub_82F0F018)
{
    static const long limit = getenv("SCDA_TRACE_L3D") ? atol(getenv("SCDA_TRACE_L3D")) : 0;
    static long calls = 0;
    // SCDA_SKIP_L3D_BUCKET=<hex mask>: A/B, bucket i with bit i set is not drawn.
    static const uint32_t skip = getenv("SCDA_SKIP_L3D_BUCKET") ? strtoul(getenv("SCDA_SKIP_L3D_BUCKET"), nullptr, 16) : 0;
    if (ctx.r4.u32 < 32 && (skip >> ctx.r4.u32 & 1))
        return;
    if (calls++ < limit)
    {
        char name[40] = {};
        for (uint32_t k = 0; k + 1 < sizeof(name) && ctx.r5.u32 && PPC_LOAD_U8(ctx.r5.u32 + k); k++)
            name[k] = char(PPC_LOAD_U8(ctx.r5.u32 + k));
        RuntimeLog("[l3d] bucket %u '%s' count %d\n", ctx.r4.u32, name,
                   int32_t(PPC_LOAD_U32(ctx.r3.u32 + 20 + 12 * ctx.r4.u32)));
    }
    __imp__sub_82F0F018(ctx, base);
}

// sub_82F0EC30(P, item, bucket): one bucket item. SCDA_TRACE_L3D_ITEMS=<n>: log the item fields the
// draw path tests (u64 flags +36, byte +236, +96 material?, +28 occlusion flags, +896 samples) for
// the first n items.
extern "C" PPC_FUNC(__imp__sub_82F0EC30);

PPC_FUNC(sub_82F0EC30)
{
    static const long limit = getenv("SCDA_TRACE_L3D_ITEMS") ? atol(getenv("SCDA_TRACE_L3D_ITEMS")) : 0;
    static long calls = 0;
    if (calls++ < limit)
    {
        const uint32_t it = ctx.r4.u32;
        RuntimeLog("[l3d] item b%u 0x%08X +0 %08X +36 %08X%08X +236 %u +96 %08X +28 %08X +896 %08X +276 %08X\n",
                   ctx.r5.u32, it, PPC_LOAD_U32(it), PPC_LOAD_U32(it + 36), PPC_LOAD_U32(it + 40),
                   PPC_LOAD_U8(it + 236), PPC_LOAD_U32(it + 96), PPC_LOAD_U32(it + 28), PPC_LOAD_U32(it + 896),
                   PPC_LOAD_U32(it + 276));
    }
    __imp__sub_82F0EC30(ctx, base);
}

// sub_82EF8CA8(R, V): appends candidates { prim, u64, mask @+12 } (16 bytes, items @V+44, count
// @V+48) with prim byte +236 != 0 to the visible lists (V+156 + 12*i ...). SCDA_WATCH_L3D=2: log
// the candidate count once and watch who fills it.
extern "C" PPC_FUNC(__imp__sub_82EF8CA8);

PPC_FUNC(sub_82EF8CA8)
{
    static bool armed;
    if (!armed && getenv("SCDA_WATCH_L3D") && atoi(getenv("SCDA_WATCH_L3D")) == 2)
    {
        armed = true;
        RuntimeLog("[l3d] cull V 0x%08X candidates %d\n", ctx.r4.u32, int32_t(PPC_LOAD_U32(ctx.r4.u32 + 48)));
        WatchAdd(ctx.r4.u32 + 48, 30);
    }
    __imp__sub_82EF8CA8(ctx, base);
}

// sub_82511988(resource, ?, out, ?, flags): D3D buffer lock. Called by the visibility driver
// sub_82EF9990 (lr 0x82EF99F0) on a per-frame buffer of one byte per object that the GPU writes
// (byte & 7 != 0 -> object is a candidate). SCDA_TRACE_VISBUF=<n>: dump it for n frames.
static uint32_t g_visDriver; // V of the last sub_82EF9990 (visibility driver, see below)
static uint32_t g_lightDriver; // X of the last sub_82EF9100 (light-list reader, see below)
extern "C" PPC_FUNC(__imp__sub_82511988);

PPC_FUNC(sub_82511988)
{
    static const long limit = getenv("SCDA_TRACE_VISBUF") ? atol(getenv("SCDA_TRACE_VISBUF")) : 0;
    static long calls = 0;
    const uint32_t lr = uint32_t(ctx.lr), res = ctx.r3.u32, out = ctx.r5.u32;
    __imp__sub_82511988(ctx, base);
    uint32_t PzVisFill(uint32_t ptr, uint8_t* base);
    if (lr == 0x82EF99F0)
        PzVisFill(PPC_LOAD_U32(out + 4), base);
    // Light list (sub_82EF9100, lr 0x82EF914C): bytes at ptr + X[192], X[196] lights, written by the
    // 2nd/3rd visibility draws. Lights with bits 0+1 set join light lists X->+80 / +92 (shadow
    // lights, sphere/volume occluders). The GPU bytes are used as is: their black volumes were the
    // sphere occluders' signed k_16_16 falloff texture read as unorm, plus the shadow-mask clear
    // colour (docs/light_list.md). SCDA_VISBUF_LIGHTS=off: clear them (old: those lights unused).
    static const bool clearLights = getenv("SCDA_VISBUF_LIGHTS") && !strcmp(getenv("SCDA_VISBUF_LIGHTS"), "off");
    if (lr == 0x82EF914C && g_lightDriver && clearLights)
    {
        const uint32_t first = PPC_LOAD_U32(out + 4) + PPC_LOAD_U32(g_lightDriver + 192), n = PPC_LOAD_U32(g_lightDriver + 196);
        for (uint32_t k = 0; k < n && k < 65536; k++)
            PPC_STORE_U8(first + k, 0);
    }
    if (lr == 0x82EF99F0 && calls < limit)
    {
        calls++;
        const uint32_t p = PPC_LOAD_U32(out + 4);
        int nonzero = 0;
        for (uint32_t k = 0; k < 1024; k++)
            nonzero += PPC_LOAD_U8(p + k) != 0;
        if (g_visDriver) // what the driver loop reads: n bytes at ptr + V[184]; bits 0+1 = candidate
        {
            const uint32_t first = p + PPC_LOAD_U32(g_visDriver + 184), n = PPC_LOAD_U32(g_visDriver + 188);
            uint32_t set = 0, cand = 0;
            for (uint32_t k = 0; k < n && k < 65536; k++)
            {
                const uint8_t b = PPC_LOAD_U8(first + k);
                set += b != 0;
                cand += (b & 3) == 3;
            }
            RuntimeLog("[l3d] visbuf read +%u x%u: nonzero %u candidates %u\n", PPC_LOAD_U32(g_visDriver + 184), n, set, cand);
        }
        char line[200];
        int len = snprintf(line, sizeof(line), "[l3d] visbuf res 0x%08X (+0 %08X +4 %08X) ptr 0x%08X nonzero/1024 %d:", res,
                           PPC_LOAD_U32(res), PPC_LOAD_U32(res + 4), p, nonzero);
        for (uint32_t k = 0; k < 32; k++)
            len += snprintf(line + len, sizeof(line) - len, " %02X", PPC_LOAD_U8(p + k));
        RuntimeLog("%s\n", line);
    }
}

// Visibility driver sub_82EF9990(V): reads object byte i at ptr + V[184] + i, V[188] objects.
// Byte bits: 0 visible, 1 visible (both needed to become a draw candidate), 2-3 / 4 extra lists,
// 5-7 view/light list mask. The GPU fills them: VisibilityPass PS f374a14f971cb1bd (draws of
// sub_82EF7E90, stream constant from sub_82EF5CA8: k_8, one byte per object) tests each object's
// screen box against a downsampled depth texture and MEMEXPORTs the byte (eA = r32, eM0 = r33).
// Without memexport the bytes stayed 0 and only the always-visible skybox (sub_82EF8B18) was
// drawn. PS memexport (translate.cpp MemExport, renderer u7 window) now writes them: default = the
// GPU bytes. SCDA_VISBUF_FILL=<hex>: byte = hex after the lock (FF = no culling, the old
// workaround; the renderer then skips memexport); or<hex>: GPU byte | hex; off: GPU bytes (default).
extern "C" PPC_FUNC(__imp__sub_82EF9990);

PPC_FUNC(sub_82EF9990)
{
    g_visDriver = ctx.r3.u32;
    __imp__sub_82EF9990(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82EF9100);
PPC_FUNC(sub_82EF9100)
{
    g_lightDriver = ctx.r3.u32;
    __imp__sub_82EF9100(ctx, base);
}

uint32_t PzVisFill(uint32_t ptr, uint8_t* base)
{
    // SCDA_VISBUF_FILL (see above). Unset = off.
    static const char* env = getenv("SCDA_VISBUF_FILL");
    static const bool off = !env || !strcmp(env, "off");
    static const bool orMode = env && !strncmp(env, "or", 2);
    static const uint8_t fill = env && !off ? uint8_t(strtoul(env + (orMode ? 2 : 0), nullptr, 16)) : 0xFF;
    if (off || !g_visDriver || !ptr)
        return 0;
    const uint32_t first = ptr + PPC_LOAD_U32(g_visDriver + 184), n = PPC_LOAD_U32(g_visDriver + 188);
    static uint32_t lastFirst, maxN; // log a new buffer or a new high count only (n jitters per frame)
    if (first != lastFirst || n > maxN)
    {
        lastFirst = first;
        maxN = n;
        RuntimeLog("[l3d] visibility bytes 0x%08X x%u = %02X (SCDA_VISBUF_FILL)\n", first, n, fill);
    }
    // n bytes from first is exactly the range the driver loop reads.
    for (uint32_t k = 0; k < n; k++)
        PPC_STORE_U8(first + k, orMode ? uint8_t(PPC_LOAD_U8(first + k) | fill) : fill);
    return n;
}
