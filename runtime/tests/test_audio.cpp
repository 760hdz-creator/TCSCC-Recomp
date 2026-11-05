#include "check.h"
#include "guest_memory.h"
#include <atomic>
#include <runtime.h>

static std::atomic<uint32_t> g_audioArg, g_audioCalls, g_audioThread;

static PPC_FUNC(FakeAudioCallback)
{
    g_audioArg = ctx.r3.u32;
    g_audioCalls++;
    g_audioThread = GetCurrentThreadId();
}

TEST(audio_register_writes_driver_and_pumps_callback)
{
    TestGuestBase();
    PPC_LOOKUP_FUNC(g_base, 0x824C5000) = FakeAudioCallback;
    GuestStore32(0x40200000, 0x824C5000); // {callback, argument}
    GuestStore32(0x40200004, 0x4023EC80);
    PPCContext ctx{};
    ctx.r3.u64 = 0x40200000;
    ctx.r4.u64 = 0x40200010; // driver handle out
    __imp__XAudioRegisterRenderDriverClient(ctx, g_base);
    CHECK_EQ(ctx.r3.u64, 0ull);
    uint32_t driver = GuestLoad32(0x40200010);
    CHECK(driver != 0);
    for (int i = 0; i < 100 && g_audioCalls.load() < 3; i++)
        Sleep(5);
    CHECK(g_audioCalls.load() >= 3);
    CHECK_EQ(GuestLoad32(g_audioArg.load()), 0x4023EC80u); // r3 = cell holding the argument (Xenia)
    CHECK(g_audioThread.load() != GetCurrentThreadId());

    ctx.r3.u64 = driver;
    __imp__XAudioUnregisterRenderDriverClient(ctx, g_base);
    Sleep(30);
    uint32_t after = g_audioCalls.load();
    Sleep(50);
    CHECK_EQ(g_audioCalls.load(), after);
}

static std::atomic<uint32_t> g_audioCallsB;
static PPC_FUNC(FakeAudioCallbackB) { g_audioCallsB++; }

TEST(audio_two_clients_both_pumped)
{
    TestGuestBase();
    PPC_LOOKUP_FUNC(g_base, 0x824C5000) = FakeAudioCallback;
    PPC_LOOKUP_FUNC(g_base, 0x824C5100) = FakeAudioCallbackB;
    GuestStore32(0x40200040, 0x824C5000);
    GuestStore32(0x40200044, 0x4023EC80);
    GuestStore32(0x40200048, 0x824C5100);
    GuestStore32(0x4020004C, 0x4023ED00);
    PPCContext ctx{};
    ctx.r3.u64 = 0x40200040;
    ctx.r4.u64 = 0x40200050;
    __imp__XAudioRegisterRenderDriverClient(ctx, g_base);
    ctx.r3.u64 = 0x40200048;
    ctx.r4.u64 = 0x40200054;
    __imp__XAudioRegisterRenderDriverClient(ctx, g_base);
    const uint32_t a = GuestLoad32(0x40200050), b = GuestLoad32(0x40200054);
    CHECK(a != b);
    const uint32_t startA = g_audioCalls.load(), startB = g_audioCallsB.load();
    for (int i = 0; i < 100 && (g_audioCalls.load() < startA + 3 || g_audioCallsB.load() < startB + 3); i++)
        Sleep(5);
    CHECK(g_audioCalls.load() >= startA + 3); // the first client keeps running after the second registers
    CHECK(g_audioCallsB.load() >= startB + 3);
    ctx.r3.u64 = a;
    __imp__XAudioUnregisterRenderDriverClient(ctx, g_base);
    Sleep(30);
    const uint32_t afterA = g_audioCalls.load(), afterB = g_audioCallsB.load();
    Sleep(50);
    CHECK_EQ(g_audioCalls.load(), afterA);
    CHECK(g_audioCallsB.load() > afterB);
    ctx.r3.u64 = b;
    __imp__XAudioUnregisterRenderDriverClient(ctx, g_base);
}

TEST(audio_speaker_config_and_volume)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0x40200020;
    __imp__XAudioGetSpeakerConfig(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40200020), 0x00010001u);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 0x40200024;
    __imp__XAudioGetVoiceCategoryVolume(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40200024), 0x3F800000u); // 1.0f
}

void AudioConvertFrame(const uint8_t* guest, float* six, float* stereo);

TEST(audio_frame_planar_be_to_interleaved_and_stereo)
{
    // Planar: channel c sample i at (c * 256 + i), big-endian floats; FL=1, FR=2, C=4, BL=8.
    static uint32_t in[256 * 6];
    const float values[6] = {1, 2, 4, 0.5f, 8, 0};
    for (int c = 0; c < 6; c++)
        for (int i = 0; i < 256; i++)
        {
            uint32_t v;
            memcpy(&v, &values[c], 4);
            in[c * 256 + i] = __builtin_bswap32(v);
        }
    static float six[256 * 6], stereo[512];
    AudioConvertFrame(reinterpret_cast<const uint8_t*>(in), six, stereo);
    CHECK(six[0] == 1.0f && six[1] == 2.0f && six[3] == 0.5f && six[6 * 255 + 4] == 8.0f);
    CHECK(stereo[0] > 1 + 0.7071f * 12 - 0.01f && stereo[0] < 1 + 0.7071f * 12 + 0.01f);
    CHECK(stereo[511] > 2 + 0.7071f * 4 - 0.01f && stereo[511] < 2 + 0.7071f * 4 + 0.01f);
}
