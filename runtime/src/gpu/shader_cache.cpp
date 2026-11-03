#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <runtime.h>
#include <gpu/shader_cache.h>

static std::mutex g_shaderMutex;
static std::unordered_map<uint64_t, std::unique_ptr<ShaderBlob>> g_shaders;
static const ShaderBlob* g_current[2];

static uint64_t Fnv1a(const uint8_t* p, size_t n, uint64_t seed)
{
    uint64_t h = 1469598103934665603ull ^ seed;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// True if the control flow has an ALLOC of type memory (memexport). The translator does not
// implement memexport (eA/eM writes become plain temps), so such shaders silently lose their
// writes. CF: two 48-bit instructions per 3 dwords; opcode bits 44-47, exec address bits 0-11,
// alloc type bits 41-42 (3 = memory); the CF block ends at the lowest exec address.
static bool HasMemExport(const uint32_t* w, uint32_t dwords)
{
    uint32_t limit = ~0u, slot = 0;
    for (uint32_t i = 0; i + 2 < dwords && slot < limit; i += 3)
    {
        const uint64_t pair[2] = { (uint64_t(w[i + 1] & 0xFFFF) << 32) | w[i], (uint64_t(w[i + 2]) << 16) | (w[i + 1] >> 16) };
        for (uint64_t c : pair)
        {
            if (slot++ >= limit)
                break;
            const uint32_t op = uint32_t(c >> 44) & 0xF;
            if (op == 0xC && (uint32_t(c >> 41) & 3) == 3)
                return true;
            if ((op >= 1 && op <= 6) || op == 0xE || op == 0xF) // exec variants: ALU starts at the address
                limit = std::min(limit, uint32_t(c & 0xFFF) * 2);
        }
    }
    return false;
}

const ShaderBlob* ShaderCacheInsert(ShaderType type, const uint32_t* be, uint32_t dwords)
{
    if (!dwords || !be)
        return nullptr;
    uint64_t hash = Fnv1a((const uint8_t*)be, dwords * 4ull, uint64_t(type));
    std::lock_guard lock(g_shaderMutex);
    auto it = g_shaders.find(hash);
    if (it != g_shaders.end())
        return it->second.get();
    auto blob = std::make_unique<ShaderBlob>();
    blob->type = type;
    blob->hash = hash;
    blob->ucode.resize(dwords);
    for (uint32_t i = 0; i < dwords; i++)
        blob->ucode[i] = _byteswap_ulong(be[i]);
    // SCDA_DUMP_SHADERS=<dir>: one file per new blob (big-endian, as uploaded).
    if (const char* dir = getenv("SCDA_DUMP_SHADERS"))
    {
        std::filesystem::create_directories(dir);
        char name[64];
        snprintf(name, sizeof(name), "%016llx.%s.bin", (unsigned long long)hash, type == ShaderType::Vertex ? "vs" : "ps");
        std::ofstream(std::filesystem::path(dir) / name, std::ios::binary).write((const char*)be, dwords * 4ull);
    }
    if (HasMemExport(blob->ucode.data(), dwords))
        RuntimeLog("[shader] %016llx.%s uses MEMEXPORT (not translated: its memory writes are lost)\n",
                   (unsigned long long)hash, type == ShaderType::Vertex ? "vs" : "ps");
    const ShaderBlob* out = blob.get();
    g_shaders.emplace(hash, std::move(blob));
    return out;
}

const ShaderBlob* ShaderCacheFind(uint64_t hash)
{
    std::lock_guard lock(g_shaderMutex);
    auto it = g_shaders.find(hash);
    return it == g_shaders.end() ? nullptr : it->second.get();
}

size_t ShaderCacheSize()
{
    std::lock_guard lock(g_shaderMutex);
    return g_shaders.size();
}

const ShaderBlob* CurrentShader(ShaderType type) { return g_current[int(type) & 1]; }
void SetCurrentShader(const ShaderBlob* blob)
{
    if (blob)
        g_current[int(blob->type) & 1] = blob;
}
