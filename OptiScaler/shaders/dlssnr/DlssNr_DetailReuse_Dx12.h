#pragma once

// HispaOptiScaler v1.0.4-dev8: Modular Temporal Neural Detail Reuse (N-Frame Cadence 1:2 / 1:3).
// Evaluates nvngx_dlssnr.dll every N frames (CaptureDelta) and reprojects cached log-space neural
// detail across intermediate frames 1..N-1 (ReprojectApply) using game MotionVectors + Depth
// disocclusion rejection, delivering +35%..+45% FPS boost on top of FP8 presets.
// Automatically forces a full neural evaluation on camera cuts (Reset), resolution changes, or missing MVs.

#include "DlssNr_Dx12.h"
#include "DlssNr_DenoiseFirst_Helpers.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#define DETAIL_REUSE_NUM_HEAPS 8

struct alignas(256) DetailReuseConstants
{
    uint32_t Mode = 0; // 0 = CaptureDelta, 1 = ReprojectApply
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t MotionWidth = 0;
    uint32_t MotionHeight = 0;
    uint32_t DepthWidth = 0;
    uint32_t DepthHeight = 0;
    float MvScaleUvX = 0.0f;
    float MvScaleUvY = 0.0f;
    float ReuseBlendStrength = 0.85f;
    float MotionRejectSensitivity = 0.50f;
    uint32_t DepthInverted = 0;
};

class DlssNr_DetailReuse_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[DETAIL_REUSE_NUM_HEAPS];
    ID3D12Resource* _constantBuffers[DETAIL_REUSE_NUM_HEAPS] = {};
    uint32_t _heapIndex = 0;

    ID3D12Resource* _preNrCopy = nullptr;
    ID3D12Resource* _workColor = nullptr;
    ID3D12Resource* _detailCache[2] = { nullptr, nullptr };

    uint32_t _width = 0;
    uint32_t _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    uint32_t _activeSlot = 0;
    uint32_t _cadenceStep = 0;
    uint64_t _localEpoch = 0;
    bool _cacheValid = false;
    bool _pendingCapture = false;

    DlssNrDx12Helpers::DeferredRetireQueue _retired;

    void RetireCurrent(uint64_t epoch);
    bool EnsureResources(uint32_t width, uint32_t height, DXGI_FORMAT typedFormat, uint64_t epoch);
    bool DispatchKernel(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* baseColor, ID3D12Resource* editedOrCache,
                        ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* outCache,
                        ID3D12Resource* outColor, const DetailReuseConstants& constants);
    DetailReuseConstants BuildConstants(uint32_t mode, uint32_t w, uint32_t h, ID3D12Resource* depth,
                                        ID3D12Resource* motion, const DlssNrFrameInfo& frame) const;

  public:
    DlssNr_DetailReuse_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_DetailReuse_Dx12();

    // Returns true if intermediate frame was satisfied from reprojected cache (caller skips nvngx_dlssnr).
    // Returns false if caller must run real NR evaluation (snapshots pre-NR color for CompleteCaptureAfterNr).
    bool TryReuseOrPrepareCapture(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* targetColor,
                                  ID3D12Resource* depth, ID3D12Resource* motion, uint32_t activeWidth,
                                  uint32_t activeHeight, const DlssNrFrameInfo& frame, D3D12_RESOURCE_STATES arrival);

    // Captures log-space delta (edited - preNrCopy) + depth after real NR evaluation finishes.
    void CompleteCaptureAfterNr(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* editedColor, ID3D12Resource* depth,
                                ID3D12Resource* motion, uint32_t activeWidth, uint32_t activeHeight,
                                const DlssNrFrameInfo& frame, D3D12_RESOURCE_STATES arrival);
};
