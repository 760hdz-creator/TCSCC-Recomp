#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <runtime.h>
#include <gpu/draw_state.h>
#include <gpu/shader_compiler.h>
#include <gpu/xenos/translate.h>

// Bump when the translator or shader_common changes: old disk entries are then ignored.
static constexpr uint32_t kTranslatorVersion = 33; // v33: setp_*_push p0 from src.w, result from src.x (Xenia); v32: tx_coord_denorm normalizes by the guest size (GuestDims2D); v31: vfetch_mini inherits the previous vfetch_full's stride/source/constant; v30: texture fetch exp_adjust (TexExp); v29: colour exponent bias (g_ColorExpBias); v28: param-gen position = integer pixel; v26: PS memexport (MemExport, u7 window); v25: tx_coord_denorm fetches normalized; v24: sky nudge only for w > 0 (behind-camera vertices); v22: user clip planes (v21: far-only sky depth nudge in XenosViewport; v20: PRED_SET_INV 1 -> 0)
static constexpr uint32_t kCacheMagic = 0x53444353; // "SCDS"

struct CacheHeader
{
    uint32_t magic, version, type, size;
    uint64_t hash, checksum;
};

static std::mutex g_mutex;
static std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> g_compiled;
static uint32_t g_compileCount, g_failedCount, g_readyCount;

static uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static std::filesystem::path CachePath(const ShaderBlob& blob, const std::filesystem::path& dir)
{
    char name[64];
    snprintf(name, sizeof(name), "%016llx.%s.v%u.dxbc", (unsigned long long)blob.hash,
        blob.type == ShaderType::Pixel ? "ps" : "vs", kTranslatorVersion);
    return dir / name;
}

// Loads a cache entry; any mismatch (size, header, checksum, DXBC magic) means "not cached".
static bool LoadCached(const ShaderBlob& blob, const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    CacheHeader h{};
    if (!f || !f.read((char*)&h, sizeof(h)))
        return false;
    if (h.magic != kCacheMagic || h.version != kTranslatorVersion || h.type != uint32_t(blob.type) || h.hash != blob.hash ||
        h.size < 4 || h.size > 16u << 20)
        return false;
    out.resize(h.size);
    if (!f.read((char*)out.data(), h.size) || Fnv1a(out.data(), out.size()) != h.checksum || memcmp(out.data(), "DXBC", 4) != 0)
    {
        out.clear();
        return false;
    }
    return true;
}

static void StoreCached(const ShaderBlob& blob, const std::filesystem::path& path, const std::vector<uint8_t>& code)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    CacheHeader h{ kCacheMagic, kTranslatorVersion, uint32_t(blob.type), uint32_t(code.size()), blob.hash, Fnv1a(code.data(), code.size()) };
    // Write to a temp file and rename, so a crash mid-write never leaves a half entry under the real name.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char*)&h, sizeof(h));
        f.write((const char*)code.data(), code.size());
        if (!f)
            return;
    }
    std::filesystem::rename(tmp, path, ec);
}

static void Compile(const ShaderBlob& blob, CompiledShader& out)
{
    g_compileCount++;
    const char* stage = blob.type == ShaderType::Pixel ? "ps" : "vs";
    TranslateResult t = TranslateShader(blob);
    if (!t.ok)
    {
        g_failedCount++;
        RuntimeLog("[shader] %016llx.%s: translate failed: %s\n", (unsigned long long)blob.hash, stage, t.error.c_str());
        return;
    }
    // SCDA_DUMP_HLSL=<dir>: <hash>.vs.hlsl / <hash>.ps.hlsl for every translated shader.
    if (const char* dir = getenv("SCDA_DUMP_HLSL"))
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char name[64];
        snprintf(name, sizeof(name), "%016llx.%s.hlsl", (unsigned long long)blob.hash, stage);
        std::ofstream(std::filesystem::path(dir) / name, std::ios::binary).write(t.hlsl.data(), t.hlsl.size());
    }
    std::string err;
    out.bytecode = CompileHlsl(t.hlsl, blob.type == ShaderType::Pixel, &err);
    out.ok = !out.bytecode.empty();
    if (!out.ok)
    {
        g_failedCount++;
        RuntimeLog("[shader] %016llx.%s: compile failed: %s\n", (unsigned long long)blob.hash, stage, err.c_str());
    }
}

const CompiledShader& GetCompiledShader(const ShaderBlob& blob, const std::filesystem::path& cacheDir)
{
    std::lock_guard lock(g_mutex);
    auto it = g_compiled.find(blob.hash);
    if (it != g_compiled.end())
        return *it->second;
    auto shader = std::make_unique<CompiledShader>();
    std::filesystem::path path = CachePath(blob, cacheDir);
    // SCDA_HLSL_OVERRIDE=<dir>: a hand-edited <dir>/<hash>.<vs|ps>.hlsl replaces the translation
    // (instrumenting a shader); never cached.
    if (const char* dir = getenv("SCDA_HLSL_OVERRIDE"))
    {
        char name[64];
        snprintf(name, sizeof(name), "%016llx.%s.hlsl", (unsigned long long)blob.hash, blob.type == ShaderType::Pixel ? "ps" : "vs");
        if (std::ifstream f{ std::filesystem::path(dir) / name, std::ios::binary })
        {
            const std::string hlsl((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string err;
            shader->bytecode = CompileHlsl(hlsl, blob.type == ShaderType::Pixel, &err);
            shader->ok = !shader->bytecode.empty();
            printf("[shader] %s override: %s %s\n", name, shader->ok ? "ok" : "FAILED", err.c_str());
            if (shader->ok)
                g_readyCount++;
            return *g_compiled.emplace(blob.hash, std::move(shader)).first->second;
        }
    }
    if (LoadCached(blob, path, shader->bytecode))
        shader->ok = true;
    else
    {
        Compile(blob, *shader);
        if (shader->ok)
            StoreCached(blob, path, shader->bytecode);
    }
    if (shader->ok)
        g_readyCount++;
    return *g_compiled.emplace(blob.hash, std::move(shader)).first->second;
}

void PrecompileShaders(const std::vector<const ShaderBlob*>& blobs, const std::filesystem::path& cacheDir)
{
    // Blobs not in memory yet (deduplicated). Overrides go through GetCompiledShader as usual.
    std::vector<const ShaderBlob*> todo;
    {
        std::lock_guard lock(g_mutex);
        std::unordered_set<uint64_t> seen;
        for (const ShaderBlob* b : blobs)
            if (b && !g_compiled.count(b->hash) && seen.insert(b->hash).second)
                todo.push_back(b);
    }
    if (todo.size() < 2 || getenv("SCDA_HLSL_OVERRIDE") || getenv("SCDA_DUMP_HLSL")) // both only in Compile
        return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<CompiledShader>> out(todo.size());
    std::atomic<size_t> next{ 0 };
    std::atomic<uint32_t> compiled{ 0 }, failed{ 0 };
    auto worker = [&]
    {
        for (size_t i; (i = next++) < todo.size();)
        {
            auto shader = std::make_unique<CompiledShader>();
            const std::filesystem::path path = CachePath(*todo[i], cacheDir);
            if (LoadCached(*todo[i], path, shader->bytecode))
                shader->ok = true;
            else
            {
                compiled++;
                const char* stage = todo[i]->type == ShaderType::Pixel ? "ps" : "vs";
                TranslateResult t = TranslateShader(*todo[i]);
                std::string err;
                if (t.ok)
                    shader->bytecode = CompileHlsl(t.hlsl, todo[i]->type == ShaderType::Pixel, &err);
                shader->ok = !shader->bytecode.empty();
                if (!shader->ok)
                {
                    failed++;
                    RuntimeLog("[shader] %016llx.%s: %s failed: %s\n", (unsigned long long)todo[i]->hash, stage, t.ok ? "compile" : "translate",
                        t.ok ? err.c_str() : t.error.c_str());
                }
                else
                    StoreCached(*todo[i], path, shader->bytecode);
            }
            out[i] = std::move(shader);
        }
    };
    const size_t threads = std::min<size_t>(todo.size(), std::max(2u, std::thread::hardware_concurrency()) - 1);
    std::vector<std::thread> pool;
    for (size_t i = 1; i < threads; i++)
        pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
        t.join();
    std::lock_guard lock(g_mutex);
    g_compileCount += compiled;
    g_failedCount += failed;
    for (size_t i = 0; i < todo.size(); i++)
    {
        if (out[i]->ok)
            g_readyCount++;
        g_compiled.emplace(todo[i]->hash, std::move(out[i]));
    }
    if (compiled)
        RuntimeLog("[shader] precompiled %u (of %zu new) on %zu threads in %.0f ms\n", compiled.load(), todo.size(), threads,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

std::bitset<96> ShaderVertexFetchSlots(const ShaderBlob& blob)
{
    static std::mutex mutex;
    static std::unordered_map<uint64_t, std::bitset<96>> slots;
    std::lock_guard lock(mutex);
    auto it = slots.find(blob.hash);
    if (it == slots.end())
        it = slots.emplace(blob.hash, VertexFetchSlots(TranslateShader(blob).hlsl)).first;
    return it->second;
}

uint32_t ShaderTextureFetchSlots(const ShaderBlob& blob)
{
    static std::mutex mutex;
    static std::unordered_map<uint64_t, uint32_t> slots;
    std::lock_guard lock(mutex);
    auto it = slots.find(blob.hash);
    if (it == slots.end())
        it = slots.emplace(blob.hash, TextureFetchSlots(TranslateShader(blob).hlsl)).first;
    return it->second;
}

std::filesystem::path DefaultShaderCacheDir()
{
    if (const char* base = getenv("LOCALAPPDATA"))
        return std::filesystem::path(base) / "TCSCCRecomp" / "shadercache";
    return "shadercache";
}

uint32_t ShaderCompilerCompileCount()
{
    std::lock_guard lock(g_mutex);
    return g_compileCount;
}

uint32_t ShaderCompilerFailedCount()
{
    std::lock_guard lock(g_mutex);
    return g_failedCount;
}

uint32_t ShaderCompilerReadyCount()
{
    std::lock_guard lock(g_mutex);
    return g_readyCount;
}

void ShaderCompilerResetMemory()
{
    std::lock_guard lock(g_mutex);
    g_compiled.clear();
    g_readyCount = 0;
}
