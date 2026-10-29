#include "pch.h"
#include "DlssNr_DenoiseFirst_Dx12.h"
#include "precompile/dlssnr_denoise_first_Shader.h"

#include <Config.h>
#include <State.h>
#include <proxies/NVNGX_Proxy.h>
#include <algorithm>
#include <cmath>

using DlssNrDx12Helpers::ScopedOneToOneNgxParams;
using DlssNrDx12Helpers::ScopedStateEnvelope;
using DlssNrDx12Helpers::Transition;

DlssNr_DenoiseFirst_Dx12::DlssNr_DenoiseFirst_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice)
{
    if (!InDevice || !SetupRootSignature(InDevice, 3, 1, 1))
        return;
    const auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DenoiseFirstConstants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DENOISE_FIRST_NUM_HEAPS; ++i)
        if (FAILED(InDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&_constantBuffers[i]))))
            return;
    if (!CreateComputePipeline(InDevice, &_pipelineState, dlssnr_denoise_first_cso, sizeof(dlssnr_denoise_first_cso),
                               nullptr))
        return;
    _init = InitHeaps(InDevice, _frameHeaps, DENOISE_FIRST_NUM_HEAPS);
}

DlssNr_DenoiseFirst_Dx12::~DlssNr_DenoiseFirst_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;
    RetireCurrent(0);
    _retired.Collect(0, true);
    for (uint32_t i = 0; i < DENOISE_FIRST_NUM_HEAPS; ++i)
    {
        SAFE_RELEASE(_constantBuffers[i]);
        if (_init)
            _frameHeaps[i].ReleaseHeaps();
    }
}

void DlssNr_DenoiseFirst_Dx12::RetireCurrent(uint64_t currentEpoch)
{
    _retired.Push(currentEpoch, _cleanTex, _editedTex, _compositeTex, _ngxHandle);
    _cleanTex = _editedTex = _compositeTex = nullptr;
    _ngxHandle = nullptr;
    _width = _height = 0;
}

bool DlssNr_DenoiseFirst_Dx12::EnsureResources(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                                               uint32_t width, uint32_t height, DXGI_FORMAT typedFormat,
                                               uint32_t createFlags, bool wantRr, uint64_t currentEpoch)
{
    if (_ngxHandle && _cleanTex && _editedTex && _compositeTex && _width == width && _height == height &&
        _format == typedFormat && _flagsKey == createFlags && _isRr == wantRr)
        return true;

    RetireCurrent(currentEpoch);
    if (!NVNGXProxy::InitDx12(_device) || !NVNGXProxy::D3D12_CreateFeature() || !NVNGXProxy::D3D12_EvaluateFeature())
        return false;

    const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto texDesc =
        CD3DX12_RESOURCE_DESC::Tex2D(typedFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto initState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, initState, nullptr, IID_PPV_ARGS(&_cleanTex))) ||
        FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, initState, nullptr, IID_PPV_ARGS(&_editedTex))) ||
        FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, initState, nullptr, IID_PPV_ARGS(&_compositeTex))))
        return RetireCurrent(currentEpoch), false;

    ScopedOneToOneNgxParams oneToOne(params, width, height, _cleanTex, true);
    NVSDK_NGX_Result res = wantRr ? NVNGXProxy::D3D12_CreateFeature()(cmdList, NVSDK_NGX_Feature_RayReconstruction, params, &_ngxHandle)
                                  : NVSDK_NGX_Result_Fail;
    const bool builtRr = (res == NVSDK_NGX_Result_Success && _ngxHandle != nullptr);
    if (!builtRr)
        res = NVNGXProxy::D3D12_CreateFeature()(cmdList, NVSDK_NGX_Feature_SuperSampling, params, &_ngxHandle);
    if (res != NVSDK_NGX_Result_Success || _ngxHandle == nullptr)
    {
        LOG_WARN("DLSS-NR DenoiseFirst 1:1 CreateFeature failed: 0x{:X}", (uint32_t) res);
        return RetireCurrent(currentEpoch), false;
    }

    _width = width; _height = height; _format = typedFormat; _flagsKey = createFlags; _isRr = wantRr;
    _needsReset = true; _createEpoch = currentEpoch;
    LOG_INFO("DLSS-NR DenoiseFirst 1:1 {} ready at {}x{}", builtRr ? "RayReconstruction" : "DLAA", width, height);
    return true;
}

bool DlssNr_DenoiseFirst_Dx12::DispatchRatioTransfer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* rawColor,
                                                     const DenoiseFirstConstants& constants)
{
    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DENOISE_FIRST_NUM_HEAPS;
    auto& heap = _frameHeaps[slot];
    CreateShaderResourceView(_device, rawColor, heap.GetSrvCPU(0), _format);
    CreateShaderResourceView(_device, _editedTex, heap.GetSrvCPU(1), _format);
    CreateShaderResourceView(_device, _cleanTex, heap.GetSrvCPU(2), _format);
    CreateUnorderedAccessView(_device, _compositeTex, heap.GetUavCPU(0), 0);
    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], constants, heap.GetCbvCPU(0)))
        return false;
    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);
    return true;
}

bool DlssNr_DenoiseFirst_Dx12::EvaluateAndCompose(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                                                  DlssNr_Dx12* compose, ID3D12Resource* color, ID3D12Resource* depth,
                                                  ID3D12Resource* motion, const DlssNrFrameInfo& frame,
                                                  ID3D12CommandQueue* timingQueue)
{
    if (!_init || _failed || !cmdList || !params || !compose || !color || !depth || !motion ||
        cmdList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;

    const uint64_t epoch = ++_localEpoch;
    _retired.Collect(epoch);
    uint32_t baseX = 0, baseY = 0, createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &baseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &baseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    if (baseX != 0 || baseY != 0)
        return false;

    const auto colorDesc = color->GetDesc();
    const uint32_t rw = frame.RenderSubrectWidth ? std::min<uint32_t>(frame.RenderSubrectWidth, (uint32_t) colorDesc.Width) : (uint32_t) colorDesc.Width;
    const uint32_t rh = frame.RenderSubrectHeight ? std::min<uint32_t>(frame.RenderSubrectHeight, colorDesc.Height) : colorDesc.Height;
    const DXGI_FORMAT typedFmt = TranslateTypelessFormats(colorDesc.Format);
    if (rw < 64 || rh < 64 || typedFmt == DXGI_FORMAT_UNKNOWN)
        return false;

    const auto& cfg = *Config::Instance();
    const auto arrival = cfg.ColorResourceBarrier.has_value() ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
                                                              : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    {
        ScopedStateEnvelope env(cmdList);
        if (!EnsureResources(cmdList, params, rw, rh, typedFmt, createFlags & 0x7Fu, frame.RayReconstruction, epoch))
            return (_failed = true), false;
        if (epoch == _createEpoch)
            return true;

        Transition(cmdList, color, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, _cleanTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ScopedOneToOneNgxParams evalParams(params, rw, rh, _cleanTex, frame.Reset || _needsReset);
        if (NVNGXProxy::D3D12_EvaluateFeature()(cmdList, _ngxHandle, params, nullptr) != NVSDK_NGX_Result_Success)
        {
            Transition(cmdList, _cleanTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(cmdList, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
            return (_needsReset = true), false;
        }
        _needsReset = false;
        Transition(cmdList, _cleanTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmdList, _editedTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(_editedTex, _cleanTex);
        Transition(cmdList, _cleanTex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, _editedTex, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Transition(cmdList, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
    }

    DlssNrFrameInfo cleanFrame = frame;
    cleanFrame.ResidualAcrossRr = false;
    cleanFrame.RenderSubrectWidth = cleanFrame.OutputWidth = rw;
    cleanFrame.RenderSubrectHeight = cleanFrame.OutputHeight = rh;
    compose->Dispatch(cmdList, _editedTex, depth, motion, _editedTex, cleanFrame, timingQueue);

    ScopedStateEnvelope env(cmdList);
    float jx = 0.0f, jy = 0.0f;
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jx);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jy);
    const DenoiseFirstConstants c { rw, rh, std::isfinite(jx) ? jx : 0.0f, std::isfinite(jy) ? jy : 0.0f,
                                    std::clamp(cfg.DlssNrDenoiseFirstStrength.value_or_default(), 0.0f, 2.0f), 0.25f, 4.0f, 1u };
    Transition(cmdList, color, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _editedTex, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _compositeTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool ok = DispatchRatioTransfer(cmdList, color, c);
    if (ok)
    {
        Transition(cmdList, _compositeTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmdList, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(color, 0), src(_compositeTex, 0);
        const CD3DX12_BOX box(0, 0, 0, rw, rh, 1);
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        Transition(cmdList, color, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Transition(cmdList, _compositeTex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else
    {
        Transition(cmdList, _compositeTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
    }
    return ok;
}
