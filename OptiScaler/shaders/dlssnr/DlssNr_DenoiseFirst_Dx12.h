#pragma once

// Modular 1:1 Pre-Denoise Context for Path Tracing & Ray Reconstruction (wilsjo2 PR #123 adapted).
// Evaluates a private 1:1 NGX pass (RenderWidth x RenderHeight) on the raw noisy input prior to
// DlssNr_Dx12, feeds DlssNr_Dx12 the clean un-jittered texture, and transfers the per-pixel neural
// ratio (NR_Output / Clean_1to1) shifted by the current frame jitter (jitterX, jitterY) with
// adjustable DlssNrDenoiseFirstStrength (0.0f..2.0f = 0%..200%), anti-NaN [0.25, 4.0] clamping,
// and firefly protection back onto the raw buffer before the game's upscaler / Ray Reconstruction.

#include "DlssNr_Dx12.h"
#include "DlssNr_DenoiseFirst_Helpers.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#define DENOISE_FIRST_NUM_HEAPS 8

struct alignas(16) DenoiseFirstConstants
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    float JitterX = 0.0f;
    float JitterY = 0.0f;
    float Strength = 1.0f;
    float MinRatio = 0.25f;
    float MaxRatio = 4.0f;
    uint32_t FireflyGuard = 1;
};

class DlssNr_DenoiseFirst_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[DENOISE_FIRST_NUM_HEAPS];
    ID3D12Resource* _constantBuffers[DENOISE_FIRST_NUM_HEAPS] = {};
    uint32_t _heapIndex = 0;

    ID3D12Resource* _cleanTex = nullptr;
    ID3D12Resource* _editedTex = nullptr;
    ID3D12Resource* _compositeTex = nullptr;
    NVSDK_NGX_Handle* _ngxHandle = nullptr;

    uint32_t _width = 0;
    uint32_t _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    uint32_t _flagsKey = 0;
    bool _isRr = false;
    bool _needsReset = true;
    bool _failed = false;
    uint64_t _localEpoch = 0;
    uint64_t _createEpoch = 0;

    DlssNrDx12Helpers::DeferredRetireQueue _retired;

    void RetireCurrent(uint64_t currentEpoch);
    bool EnsureResources(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, uint32_t width,
                         uint32_t height, DXGI_FORMAT typedFormat, uint32_t createFlags, bool wantRr,
                         uint64_t currentEpoch);
    bool DispatchRatioTransfer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* rawColor,
                               const DenoiseFirstConstants& constants);

  public:
    DlssNr_DenoiseFirst_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_DenoiseFirst_Dx12();

    // Runs 1:1 pre-clean + DlssNr_Dx12 + jitter-shifted ratio transfer onto color.
    bool EvaluateAndCompose(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, DlssNr_Dx12* compose,
                            ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                            const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue);
};
