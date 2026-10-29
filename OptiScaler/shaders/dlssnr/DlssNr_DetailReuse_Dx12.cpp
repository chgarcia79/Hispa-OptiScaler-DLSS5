#include "pch.h"
#include "DlssNr_DetailReuse_Dx12.h"
#include "precompile/dlssnr_detail_reuse_Shader.h"

#include <Config.h>
#include <State.h>
#include <algorithm>
#include <cmath>

using DlssNrDx12Helpers::CopySubRegion;
using DlssNrDx12Helpers::CreateSafeSrv;
using DlssNrDx12Helpers::ResolveSrvFormat;
using DlssNrDx12Helpers::ScopedStateEnvelope;
using DlssNrDx12Helpers::Transition;

DlssNr_DetailReuse_Dx12::DlssNr_DetailReuse_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice)
{
    if (!InDevice)
        return;
    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    if (!SetupRootSignature(InDevice, 4, 2, 1, 0, 0, 1, &linearClamp))
        return;
    const auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DetailReuseConstants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DETAIL_REUSE_NUM_HEAPS; ++i)
        if (FAILED(InDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_constantBuffers[i]))))
            return;
    if (!CreateComputePipeline(InDevice, &_pipelineState, dlssnr_detail_reuse_cso, sizeof(dlssnr_detail_reuse_cso), nullptr))
        return;
    _init = InitHeaps(InDevice, _frameHeaps, DETAIL_REUSE_NUM_HEAPS);
}

DlssNr_DetailReuse_Dx12::~DlssNr_DetailReuse_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;
    RetireCurrent(0);
    _retired.Collect(0, true);
    for (uint32_t i = 0; i < DETAIL_REUSE_NUM_HEAPS; ++i)
    {
        SAFE_RELEASE(_constantBuffers[i]);
        if (_init)
            _frameHeaps[i].ReleaseHeaps();
    }
}

void DlssNr_DetailReuse_Dx12::RetireCurrent(uint64_t epoch)
{
    _retired.Push(epoch, _preNrCopy, _workColor, _detailCache[0]);
    _retired.Push(epoch, _detailCache[1], nullptr, nullptr);
    _preNrCopy = _workColor = _detailCache[0] = _detailCache[1] = nullptr;
    _width = _height = 0;
    _cacheValid = _pendingCapture = false;
}

bool DlssNr_DetailReuse_Dx12::EnsureResources(uint32_t width, uint32_t height, DXGI_FORMAT typedFormat, uint64_t epoch)
{
    if (_preNrCopy && _workColor && _detailCache[0] && _detailCache[1] && _width == width && _height == height && _format == typedFormat)
        return true;

    RetireCurrent(epoch);
    const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto copyDesc = CD3DX12_RESOURCE_DESC::Tex2D(typedFormat, width, height, 1, 1);
    const auto workDesc = CD3DX12_RESOURCE_DESC::Tex2D(typedFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto cacheDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto initSt = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &copyDesc, initSt, nullptr, IID_PPV_ARGS(&_preNrCopy))) ||
        FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &workDesc, initSt, nullptr, IID_PPV_ARGS(&_workColor))) ||
        FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &cacheDesc, initSt, nullptr, IID_PPV_ARGS(&_detailCache[0]))) ||
        FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &cacheDesc, initSt, nullptr, IID_PPV_ARGS(&_detailCache[1]))))
        return RetireCurrent(epoch), false;

    _width = width; _height = height; _format = typedFormat;
    _activeSlot = _cadenceStep = 0;
    return true;
}

DetailReuseConstants DlssNr_DetailReuse_Dx12::BuildConstants(uint32_t mode, uint32_t w, uint32_t h, ID3D12Resource* depth,
                                                             ID3D12Resource* motion, const DlssNrFrameInfo& frame) const
{
    const auto& cfg = *Config::Instance();
    const auto mDesc = motion->GetDesc(), dDesc = depth->GetDesc();
    const uint32_t subW = frame.RenderSubrectWidth ? frame.RenderSubrectWidth : w;
    const uint32_t subH = frame.RenderSubrectHeight ? frame.RenderSubrectHeight : h;
    const uint32_t mw = (frame.MotionVectorsLowResolution || (uint32_t) mDesc.Width < w) ? std::min<uint32_t>(subW, (uint32_t) mDesc.Width) : std::min<uint32_t>(w, (uint32_t) mDesc.Width);
    const uint32_t mh = (frame.MotionVectorsLowResolution || mDesc.Height < h) ? std::min<uint32_t>(subH, mDesc.Height) : std::min<uint32_t>(h, mDesc.Height);
    const uint32_t dw = ((uint32_t) dDesc.Width < w) ? std::min<uint32_t>(subW, (uint32_t) dDesc.Width) : std::min<uint32_t>(w, (uint32_t) dDesc.Width);
    const uint32_t dh = (dDesc.Height < h) ? std::min<uint32_t>(subH, dDesc.Height) : std::min<uint32_t>(h, dDesc.Height);

    return DetailReuseConstants {
        mode, w, h, mw, mh, dw, dh,
        (mw > 0 && std::isfinite(frame.MvScaleX)) ? (frame.MvScaleX / (float) mw) : 0.0f,
        (mh > 0 && std::isfinite(frame.MvScaleY)) ? (frame.MvScaleY / (float) mh) : 0.0f,
        std::clamp(cfg.DlssNrDetailReuseBlend.value_or_default(), 0.0f, 1.0f),
        std::clamp(cfg.DlssNrDetailReuseMotionReject.value_or_default(), 0.0f, 1.0f),
        frame.DepthInverted ? 1u : 0u
    };
}

bool DlssNr_DetailReuse_Dx12::DispatchKernel(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* baseColor,
                                             ID3D12Resource* editedOrCache, ID3D12Resource* motion, ID3D12Resource* depth,
                                             ID3D12Resource* outCache, ID3D12Resource* outColor, const DetailReuseConstants& c)
{
    auto& heap = _frameHeaps[_heapIndex];
    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DETAIL_REUSE_NUM_HEAPS;
    if (!CreateSafeSrv(_device, baseColor, heap.GetSrvCPU(0), _format) || !CreateSafeSrv(_device, editedOrCache, heap.GetSrvCPU(1)) ||
        !CreateSafeSrv(_device, motion, heap.GetSrvCPU(2)) || !CreateSafeSrv(_device, depth, heap.GetSrvCPU(3)))
        return false;
    CreateUnorderedAccessView(_device, outCache, heap.GetUavCPU(0), 0);
    CreateUnorderedAccessView(_device, outColor, heap.GetUavCPU(1), 0);
    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], c, heap.GetCbvCPU(0)))
        return false;
    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch((c.Width + 7) / 8, (c.Height + 7) / 8, 1);
    return true;
}

bool DlssNr_DetailReuse_Dx12::TryReuseOrPrepareCapture(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* targetColor,
                                                       ID3D12Resource* depth, ID3D12Resource* motion, uint32_t activeWidth,
                                                       uint32_t activeHeight, const DlssNrFrameInfo& frame, D3D12_RESOURCE_STATES arrival)
{
    _pendingCapture = false;
    const auto& cfg = *Config::Instance();
    const uint32_t interval = std::clamp(cfg.DlssNrDetailReuseInterval.value_or_default(), 1u, 4u);
    if (!_init || !cfg.DlssNrDetailReuseEnabled.value_or_default() || interval < 2 || frame.ResidualAcrossRr ||
        cfg.DlssNrHoldFrame.value_or_default() || cfg.DlssNrCompare.value_or_default() != 0 || cfg.DlssNrDebugView.value_or_default() != 0 ||
        !cmdList || !targetColor || !depth || !motion || activeWidth < 64 || activeHeight < 64 || cmdList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return (_cacheValid = false), (_cadenceStep = 0), false;

    const uint64_t epoch = ++_localEpoch;
    _retired.Collect(epoch);
    const DXGI_FORMAT typedFmt = ResolveSrvFormat(targetColor->GetDesc().Format);
    if (typedFmt == DXGI_FORMAT_UNKNOWN)
        return false;
    if (frame.Reset || activeWidth != _width || activeHeight != _height || typedFmt != _format)
        _cacheValid = false, _cadenceStep = 0;

    ScopedStateEnvelope env(cmdList);
    if (!EnsureResources(activeWidth, activeHeight, typedFmt, epoch))
        return false;

    if (_cacheValid && _cadenceStep > 0 && _cadenceStep < interval)
    {
        const uint32_t nextSlot = 1u - _activeSlot;
        Transition(cmdList, targetColor, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, _detailCache[nextSlot], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(cmdList, _workColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool ok = DispatchKernel(cmdList, targetColor, _detailCache[_activeSlot], motion, depth, _detailCache[nextSlot],
                                       _workColor, BuildConstants(1u, activeWidth, activeHeight, depth, motion, frame));
        Transition(cmdList, _detailCache[nextSlot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, _workColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, targetColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        if (ok)
        {
            CopySubRegion(cmdList, targetColor, arrival, _workColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, activeWidth, activeHeight);
            _activeSlot = nextSlot;
            _cadenceStep = (_cadenceStep + 1u) % interval;
            return true;
        }
        _cacheValid = false; _cadenceStep = 0;
    }

    CopySubRegion(cmdList, _preNrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, targetColor, arrival, activeWidth, activeHeight);
    _pendingCapture = true;
    return false;
}

void DlssNr_DetailReuse_Dx12::CompleteCaptureAfterNr(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* editedColor,
                                                     ID3D12Resource* depth, ID3D12Resource* motion, uint32_t activeWidth,
                                                     uint32_t activeHeight, const DlssNrFrameInfo& frame, D3D12_RESOURCE_STATES arrival)
{
    if (!_pendingCapture || !_init || !cmdList || !editedColor || !depth || !motion || activeWidth != _width || activeHeight != _height)
        return;
    _pendingCapture = false;
    const uint32_t interval = std::clamp(Config::Instance()->DlssNrDetailReuseInterval.value_or_default(), 2u, 4u);
    const uint32_t nextSlot = 1u - _activeSlot;
    ScopedStateEnvelope env(cmdList);
    Transition(cmdList, editedColor, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _detailCache[nextSlot], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, _workColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool ok = DispatchKernel(cmdList, _preNrCopy, editedColor, motion, depth, _detailCache[nextSlot], _workColor,
                                   BuildConstants(0u, activeWidth, activeHeight, depth, motion, frame));
    Transition(cmdList, _workColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _detailCache[nextSlot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, editedColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
    if ((_cacheValid = ok))
        _activeSlot = nextSlot, _cadenceStep = 1u % interval;
}
