// SMAA 1x (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", iryoku/smaa, MIT:
// gpu/smaa/LICENSE.txt): luma edge detection, blending weight calculation, neighbourhood blending.
// Runs on the presented frame at the internal resolution (AntiAliasing=SMAA, D3D11Renderer::BlitPost).
#pragma once
#include <cstdint>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>

class Smaa
{
public:
    // Compiles the passes and uploads the area/search lookup textures; false (error filled) on failure.
    bool Init(ID3D11Device* device, std::string* error);
    bool Ready() const { return m_ready; }
    // source: gamma-space colour, width x height; writes the antialiased frame into target (same size).
    // Leaves the context's pipeline state changed (the caller rebinds what it needs).
    void Run(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height);

private:
    bool MakeTargets(uint32_t width, uint32_t height);
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    ID3D11Device* m_device = nullptr;
    bool m_ready = false;
    Ptr<ID3D11VertexShader> m_vs[3];
    Ptr<ID3D11PixelShader> m_ps[3];
    Ptr<ID3D11ShaderResourceView> m_area, m_search;
    Ptr<ID3D11SamplerState> m_linear, m_point;
    Ptr<ID3D11Buffer> m_constants;
    Ptr<ID3D11RasterizerState> m_rasterizer;
    uint32_t m_width = 0, m_height = 0;
    Ptr<ID3D11Texture2D> m_edgesTex, m_blendTex;
    Ptr<ID3D11RenderTargetView> m_edgesRtv, m_blendRtv;
    Ptr<ID3D11ShaderResourceView> m_edgesSrv, m_blendSrv;
};
