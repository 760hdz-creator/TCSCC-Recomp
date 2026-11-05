// Player card on the main menu page (top right): the Steam avatar (or the name's initial), the profile
// name and the online status. Drawn by the overlay (render thread) only while the menu's own IndexPage
// is up (main_menu.cpp), the F1 menu closed and the front end running. Game build only (not in tests).
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>
#include <online_identity.h>
#include "online/steam_net.h"
#include "online/xnet.h"
#include "overlay.h"

namespace
{
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_avatar;
int g_avatarPolls = 0;

void LoadAvatar(ID3D11Device* device)
{
    // Polled every 30th frame until the helper's kAvatar frame arrived.
    if (g_avatar || !device || (g_avatarPolls++ % 30) != 0)
        return;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!steamnet::LocalAvatar(rgba, w, h))
        return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ rgba.data(), w * 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(device->CreateTexture2D(&td, &data, &tex)))
        device->CreateShaderResourceView(tex.Get(), nullptr, &g_avatar);
}

void Draw(ImFont* body, ID3D11Device* device)
{
    LoadAvatar(device);
    // Layout in the game's 1056x576 page units, in the 16:9 frame inside the window (as recomp_page.cpp).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = vp->Size.x, h = vp->Size.y;
    if (w > h * 16.0f / 9.0f)
        w = h * 16.0f / 9.0f;
    else
        h = w * 9.0f / 16.0f;
    const float x0 = vp->Pos.x + (vp->Size.x - w) * 0.5f, y0 = vp->Pos.y + (vp->Size.y - h) * 0.5f, k = h / 576.0f;
    auto P = [&](float x, float y) { return ImVec2(x0 + x * k, y0 + y * k); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const online::Identity id = online::GetIdentity();
    const bool enabled = xnet::Enabled(), steam = enabled && steamnet::Running();
    const char* status = !enabled ? "Offline" : steam ? "Online · Steam" : "Local only · Steam not running";
    const ImU32 statusColor = !enabled ? IM_COL32(150, 150, 150, 255) : steam ? IM_COL32(90, 200, 90, 255) : IM_COL32(220, 170, 60, 255);

    // Anchored to the frame's right edge (its width in page units is w / k, not 1056: 576 rows are not 16:9).
    const float kRight = w / k - 20.0f;
    constexpr float kTop = 20.0f, kH = 46.0f, kAv = 36.0f, kPad = 5.0f;
    const float nameW = body->CalcTextSizeA(17.0f * k, FLT_MAX, 0.0f, id.gamertag.c_str()).x / k;
    const float statusW = body->CalcTextSizeA(12.0f * k, FLT_MAX, 0.0f, status).x / k + 12.0f;
    const float cardW = kPad * 3 + kAv + std::max(nameW, statusW) + 8.0f, left = kRight - cardW;
    dl->AddRectFilled(P(left, kTop), P(kRight, kTop + kH), IM_COL32(8, 8, 8, 200), 3.0f * k);
    dl->AddRectFilled(P(left, kTop), P(left + 2.5f, kTop + kH), IM_COL32(176, 8, 8, 255)); // the menu's red accent

    const ImVec2 a0 = P(left + kPad + 3, kTop + kPad), a1 = P(left + kPad + 3 + kAv, kTop + kPad + kAv);
    if (g_avatar)
        dl->AddImage(ImTextureID(g_avatar.Get()), a0, a1);
    else
    {
        dl->AddRectFilled(a0, a1, IM_COL32(60, 60, 60, 255));
        const char initial[2] = { id.gamertag.empty() ? 'P' : id.gamertag[0], 0 };
        const ImVec2 sz = body->CalcTextSizeA(22.0f * k, FLT_MAX, 0.0f, initial);
        dl->AddText(body, 22.0f * k, ImVec2((a0.x + a1.x - sz.x) * 0.5f, (a0.y + a1.y - sz.y) * 0.5f), IM_COL32(230, 230, 230, 255), initial);
    }
    const float tx = left + kPad * 2 + 3 + kAv;
    dl->AddText(body, 17.0f * k, P(tx, kTop + 4), IM_COL32(255, 255, 255, 255), id.gamertag.c_str());
    dl->AddCircleFilled(P(tx + 4, kTop + 33), 3.0f * k, statusColor);
    dl->AddText(body, 12.0f * k, P(tx + 11, kTop + 26), IM_COL32(200, 200, 200, 255), status);
}

struct Register
{
    Register() { OverlaySetPlayerCard(&Draw); }
} g_register;
} // namespace
