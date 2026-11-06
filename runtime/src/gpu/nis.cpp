#include <gpu/nis.h>
#include <d3dcompiler.h>
#include <cmath>
#include <gpu/nis/NIS_Config.h>

namespace
{
const char kNisScaler[] =
#include <gpu/nis/nis_hlsl.inc>
    ;

// NIS_Main.hlsl (the SDK's HLSL entry point), with the block size for the generic profile.
const char kMain[] = R"HLSL(
#define NIS_HLSL 1
#define NIS_SCALER 1
#define NIS_HDR_MODE 0
#define NIS_BLOCK_WIDTH 32
#define NIS_BLOCK_HEIGHT 24
#define NIS_THREAD_GROUP_SIZE 256
cbuffer cb : register(b0)
{
    float kDetectRatio; float kDetectThres; float kMinContrastRatio; float kRatioNorm;
    float kContrastBoost; float kEps; float kSharpStartY; float kSharpScaleY;
    float kSharpStrengthMin; float kSharpStrengthScale; float kSharpLimitMin; float kSharpLimitScale;
    float kScaleX; float kScaleY; float kDstNormX; float kDstNormY; float kSrcNormX; float kSrcNormY;
    uint kInputViewportOriginX; uint kInputViewportOriginY; uint kInputViewportWidth; uint kInputViewportHeight;
    uint kOutputViewportOriginX; uint kOutputViewportOriginY; uint kOutputViewportWidth; uint kOutputViewportHeight;
    float reserved0; float reserved1;
};
SamplerState samplerLinearClamp : register(s0);
Texture2D in_texture : register(t0);
RWTexture2D<float4> out_texture : register(u0);
Texture2D coef_scaler : register(t1);
Texture2D coef_usm : register(t2);
)HLSL";

const char kEntry[] = R"HLSL(
[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)
{
    NVScaler(blockIdx.xy, threadIdx.x);
}
)HLSL";

constexpr uint32_t kBlockWidth = 32, kBlockHeight = 24;
} // namespace

bool Nis::Supports(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH)
{
    return inW && inH && outW >= inW && outH >= inH && outW <= inW * 2 && outH <= inH * 2;
}

bool Nis::Init(ID3D11Device* device, std::string* error)
{
    const std::string source = std::string(kMain) + kNisScaler + kEntry;
    Ptr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "NIS_Main.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        if (error)
            *error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
        return false;
    }
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_cs)))
        return false;
    // Coefficient tables: kPhaseCount rows of kFilterSize floats, as RGBA32F texels (the SDK sample's layout).
    auto table = [&](const float (*coef)[kFilterSize], Ptr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFilterSize / 4;
        td.Height = kPhaseCount;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{ coef, kFilterSize * sizeof(float), 0 };
        Ptr<ID3D11Texture2D> tex;
        return SUCCEEDED(device->CreateTexture2D(&td, &data, &tex)) && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
    };
    if (!table(coef_scale, m_coefScale) || !table(coef_usm, m_coefUsm))
        return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &m_linear)))
        return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (sizeof(NISConfig) + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_constants)))
        return false;
    m_ready = true;
    return true;
}

bool Nis::Run(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, uint32_t inW, uint32_t inH,
              ID3D11UnorderedAccessView* target, uint32_t outW, uint32_t outH, float sharpness)
{
    if (!m_ready || !Supports(inW, inH, outW, outH))
        return false;
    NISConfig config{};
    if (!NVScalerUpdateConfig(config, sharpness, 0, 0, inW, inH, inW, inH, 0, 0, outW, outH, outW, outH, NISHDRMode::None))
        return false;
    alignas(16) uint8_t buffer[(sizeof(NISConfig) + 15) & ~15u] = {};
    memcpy(buffer, &config, sizeof(config));
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, buffer, 0, 0);
    ID3D11ShaderResourceView* srvs[3] = { source, m_coefScale.Get(), m_coefUsm.Get() };
    context->CSSetShader(m_cs.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 3, srvs);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->CSSetSamplers(0, 1, m_linear.GetAddressOf());
    context->CSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    context->Dispatch((outW + kBlockWidth - 1) / kBlockWidth, (outH + kBlockHeight - 1) / kBlockHeight, 1);
    ID3D11ShaderResourceView* none[3] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    context->CSSetShaderResources(0, 3, none);
    context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    return true;
}
