// NVIDIA Image Scaling (NIS) 1.0.3 (NVIDIAGameWorks/NVIDIAImageScaling, MIT: gpu/nis/LICENSE.txt):
// spatial upscaler + sharpener, one compute pass. [Display] Upscaler=NIS (D3D11Renderer::BlitPost);
// NIS only scales up to 2x per axis, larger ratios keep Lanczos.
#pragma once
#include <cstdint>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>

class Nis
{
public:
    // Compiles the shader and uploads the filter coefficient tables; false (error filled) on failure.
    bool Init(ID3D11Device* device, std::string* error);
    bool Ready() const { return m_ready; }
    // A ratio NIS takes (output / input between 1 and 2 on both axes).
    static bool Supports(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);
    // Upscales source (inW x inH, gamma-space colour) into target (outW x outH, typed UAV), sharpness 0..1.
    // Leaves the context's compute state changed.
    bool Run(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, uint32_t inW, uint32_t inH,
             ID3D11UnorderedAccessView* target, uint32_t outW, uint32_t outH, float sharpness);

private:
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    bool m_ready = false;
    Ptr<ID3D11ComputeShader> m_cs;
    Ptr<ID3D11ShaderResourceView> m_coefScale, m_coefUsm;
    Ptr<ID3D11SamplerState> m_linear;
    Ptr<ID3D11Buffer> m_constants;
};
