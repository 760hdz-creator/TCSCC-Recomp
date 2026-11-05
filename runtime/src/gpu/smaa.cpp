#include <gpu/smaa.h>
#include <gpu/xenos/translate.h>
#include <gpu/smaa/AreaTex.h>
#include <gpu/smaa/SearchTex.h>

namespace
{
const char kSmaaHlsl[] =
#include <gpu/smaa/smaa_hlsl.inc>
    ;

// Platform layer for SMAA.hlsl (SMAA_CUSTOM_SL: explicit registers instead of effect-file samplers).
const char kPrelude[] = R"HLSL(
cbuffer SmaaConstants : register(b0) { float4 smaaRtMetrics; };
#define SMAA_RT_METRICS smaaRtMetrics
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
)HLSL";

// One full-screen triangle per pass; PASS 0 = edges, 1 = blending weights, 2 = neighbourhood blending.
const char kPasses[] = R"HLSL(
Texture2D colorTex : register(t0);
Texture2D edgesTex : register(t1);
Texture2D blendTex : register(t2);
Texture2D areaTex : register(t3);
Texture2D searchTex : register(t4);
struct V
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 pix : TEXCOORD1;
    float4 o0 : TEXCOORD2;
    float4 o1 : TEXCOORD3;
    float4 o2 : TEXCOORD4;
};
#ifdef SCDA_VERTEX
V main(uint id : SV_VertexID)
{
    V v = (V)0;
    v.uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    float4 o[3] = { float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0) };
#if PASS == 0
    SMAAEdgeDetectionVS(v.uv, o);
#elif PASS == 1
    SMAABlendingWeightCalculationVS(v.uv, v.pix, o);
#else
    SMAANeighborhoodBlendingVS(v.uv, o[0]);
#endif
    v.o0 = o[0];
    v.o1 = o[1];
    v.o2 = o[2];
    return v;
}
#else
float4 main(V v) : SV_Target
{
    float4 o[3] = { v.o0, v.o1, v.o2 };
#if PASS == 0
    return float4(SMAALumaEdgeDetectionPS(v.uv, o, colorTex), 0, 0);
#elif PASS == 1
    return SMAABlendingWeightCalculationPS(v.uv, v.pix, o, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
#else
    return SMAANeighborhoodBlendingPS(v.uv, v.o0, colorTex, blendTex);
#endif
}
#endif
)HLSL";

template <class T>
bool MakeLookup(ID3D11Device* device, const unsigned char* bytes, uint32_t width, uint32_t height, uint32_t pitch, DXGI_FORMAT format, T* srv)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ bytes, pitch, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    return SUCCEEDED(device->CreateTexture2D(&td, &data, &tex)) && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, srv));
}
} // namespace

bool Smaa::Init(ID3D11Device* device, std::string* error)
{
    m_device = device;
    m_ready = false;
    m_width = m_height = 0;
    for (int pass = 0; pass < 3; pass++)
    {
        const std::string source = std::string("#define PASS ") + char('0' + pass) + "\n" + kPrelude + kSmaaHlsl + kPasses;
        auto vs = CompileHlsl(source, false, error);
        if (vs.empty() || FAILED(device->CreateVertexShader(vs.data(), vs.size(), nullptr, &m_vs[pass])))
            return false;
        auto ps = CompileHlsl(source, true, error);
        if (ps.empty() || FAILED(device->CreatePixelShader(ps.data(), ps.size(), nullptr, &m_ps[pass])))
            return false;
    }
    if (!MakeLookup(device, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM, m_area.GetAddressOf()) ||
        !MakeLookup(device, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM, m_search.GetAddressOf()))
    {
        if (error)
            *error = "lookup textures";
        return false;
    }
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (FAILED(device->CreateSamplerState(&sd, &m_linear)))
        return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(device->CreateSamplerState(&sd, &m_point)))
        return false;
    D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_constants)))
        return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rd, &m_rasterizer)))
        return false;
    m_ready = true;
    return true;
}

bool Smaa::MakeTargets(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height && m_edgesRtv)
        return true;
    m_width = m_height = 0;
    auto make = [&](DXGI_FORMAT format, Ptr<ID3D11Texture2D>& tex, Ptr<ID3D11RenderTargetView>& rtv, Ptr<ID3D11ShaderResourceView>& srv)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        tex.Reset(), rtv.Reset(), srv.Reset();
        return SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(m_device->CreateRenderTargetView(tex.Get(), nullptr, &rtv)) &&
               SUCCEEDED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
    };
    if (!make(DXGI_FORMAT_R8G8_UNORM, m_edgesTex, m_edgesRtv, m_edgesSrv) || !make(DXGI_FORMAT_R8G8B8A8_UNORM, m_blendTex, m_blendRtv, m_blendSrv))
        return false;
    m_width = width;
    m_height = height;
    return true;
}

void Smaa::Run(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height)
{
    if (!m_ready || !MakeTargets(width, height))
        return;
    const float metrics[4] = { 1.0f / float(width), 1.0f / float(height), float(width), float(height) };
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, metrics, 0, 0);
    const float zero[4] = {};
    context->ClearRenderTargetView(m_edgesRtv.Get(), zero);
    context->ClearRenderTargetView(m_blendRtv.Get(), zero);
    const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
    context->RSSetViewports(1, &vp);
    context->RSSetState(m_rasterizer.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    ID3D11SamplerState* samplers[2] = { m_linear.Get(), m_point.Get() };
    context->PSSetSamplers(0, 2, samplers);
    ID3D11ShaderResourceView* nones[5] = {};
    auto pass = [&](int i, ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* const (&srvs)[5])
    {
        context->PSSetShaderResources(0, 5, nones);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->PSSetShaderResources(0, 5, srvs);
        context->VSSetShader(m_vs[i].Get(), nullptr, 0);
        context->PSSetShader(m_ps[i].Get(), nullptr, 0);
        context->Draw(3, 0);
    };
    pass(0, m_edgesRtv.Get(), { source, nullptr, nullptr, nullptr, nullptr });
    pass(1, m_blendRtv.Get(), { nullptr, m_edgesSrv.Get(), nullptr, m_area.Get(), m_search.Get() });
    pass(2, target, { source, nullptr, m_blendSrv.Get(), nullptr, nullptr });
    context->PSSetShaderResources(0, 5, nones);
    ID3D11RenderTargetView* noRtv = nullptr;
    context->OMSetRenderTargets(1, &noRtv, nullptr);
}
