#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_Nvfp4Bridge.h>
#include <shaders/dlssnr/DlssNr_Retire.h>

#include <algorithm>
#include <cmath>
#include <d3d12.h>
#include <wrl/client.h>

namespace DlssNr::CompareBridge
{
struct State
{
    Microsoft::WRL::ComPtr<ID3D12Resource> answer;
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    std::shared_ptr<void> host;
    ID3D12Device* device = nullptr;
    unsigned int preset = 0;
    unsigned int workW = 0;
    unsigned int workH = 0;
    unsigned int outW = 0;
    unsigned int outH = 0;
    DXGI_FORMAT colorFmt = DXGI_FORMAT_UNKNOWN;
    bool reset = true;
};

inline State& GetState()
{
    static State s;
    return s;
}

inline void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to)
{
    if (from == to || res == nullptr)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

inline bool EnsureTextures(ID3D12Device* device, unsigned int workW, unsigned int workH, unsigned int outW,
                           unsigned int outH, DXGI_FORMAT outFmt)
{
    auto& s = GetState();
    if (s.device == device && s.workW == workW && s.workH == workH && s.outW == outW && s.outH == outH &&
        s.colorFmt == outFmt && s.answer && s.color)
        return true;

    s.answer.Reset();
    s.color.Reset();
    s.workW = workW;
    s.workH = workH;
    s.outW = outW;
    s.outH = outH;
    s.colorFmt = outFmt;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    desc.Width = workW;
    desc.Height = workH;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&s.answer))))
        return false;

    desc.Width = outW;
    desc.Height = outH;
    desc.Format = outFmt;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&s.color))))
    {
        s.answer.Reset();
        return false;
    }
    return true;
}

template <typename DispatchFn, typename TransitionTargetFn>
inline void ApplyOtherSideIfEnabled(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const Config& cfg,
                                    const DlssNrConstants& baseResolve, ID3D12Resource* modelInput,
                                    ID3D12Resource* resolveProxy, ID3D12Resource* resolveOriginal,
                                    ID3D12Resource* motionIn, ID3D12Resource* exposureTex, ID3D12Resource* depthIn,
                                    ID3D12Resource* target, unsigned int workWidth, unsigned int workHeight,
                                    unsigned int width, unsigned int height, float mvScaleX, float mvScaleY,
                                    DispatchFn&& dispatchResolve, TransitionTargetFn&& transitionTarget,
                                    D3D12_RESOURCE_STATES currentTargetState)
{
    const unsigned int compareModel = cfg.DlssNrCompareOtherModel.value_or_default();
    if (baseResolve.CompareMode == 0 || compareModel == 0)
    {
        DlssNr::Nvfp4::SetComparison(0, "");
        return;
    }
    if (!DlssNr::Nvfp4::UsesNativeHost(compareModel))
    {
        DlssNr::Nvfp4::SetComparison(0, "Solo modelos INT8/NVFP4 soportados en Comparador A/B");
        return;
    }

    auto& s = GetState();
    const unsigned int resolvedPreset = DlssNr::Nvfp4::Current(compareModel);
    const DXGI_FORMAT outFmt = target->GetDesc().Format;
    if (!EnsureTextures(device, workWidth, workHeight, width, height, outFmt))
    {
        DlssNr::Nvfp4::SetComparison(0, "No se pudieron crear texturas de comparacion A/B");
        return;
    }

    if (!s.host || s.device != device || s.preset != resolvedPreset || s.workW != workWidth || s.workH != workHeight)
    {
        s.host = DlssNr::Nvfp4::CreateHost(device, resolvedPreset, workWidth, workHeight,
                                           DlssNr::Nvfp4Bridge::AssetsPath());
        s.device = device;
        s.preset = resolvedPreset;
        s.reset = true;
    }
    if (!s.host)
    {
        DlssNr::Nvfp4::SetComparison(0, "No se pudo inicializar el modelo A/B secundario");
        return;
    }

    DlssNr::Nvfp4::HostPass pass {};
    pass.colour = modelInput;
    pass.motion = motionIn;
    pass.output = s.answer.Get();
    pass.style = cfg.DlssNrStyle.value_or_default();
    pass.intensity = cfg.DlssNrIntensity.value_or_default();
    pass.structure = cfg.DlssNrLocalStructure.value_or_default();
    pass.tone = cfg.DlssNrLocalTone.value_or_default();
    pass.skin = cfg.DlssNrSkinStructure.value_or_default();
    pass.autoMask = cfg.DlssNrAutoMask.value_or_default();
    pass.mvScaleX = mvScaleX;
    pass.mvScaleY = mvScaleY;
    pass.reset = s.reset;
    if (!DlssNr::Nvfp4::RunHost(s.host, cmdList, pass))
    {
        DlssNr::Nvfp4::SetComparison(0, "Fallo en evaluacion del modelo secundario A/B");
        return;
    }
    s.reset = false;

    Barrier(cmdList, s.answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNrConstants cmpResolve = baseResolve;
    cmpResolve.CompareMode = 0;
    const bool ok = dispatchResolve(cmdList, cmpResolve, resolveProxy, s.answer.Get(), resolveOriginal, motionIn,
                                    exposureTex, s.color.Get(), nullptr, depthIn);
    Barrier(cmdList, s.answer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!ok)
    {
        DlssNr::Nvfp4::SetComparison(0, "Fallo en resolve del modelo secundario A/B");
        return;
    }

    const float split =
        baseResolve.CompareMode == 1 ? 0.5f : std::clamp(baseResolve.CompareSplit, 0.0f, 1.0f);
    const unsigned int boundary =
        static_cast<unsigned int>(std::clamp(std::ceil(split * static_cast<float>(width) - 0.5f), 0.0f,
                                             static_cast<float>(width)));
    const bool otherOnLeft = baseResolve.CompareSwap == 0;
    D3D12_BOX box { otherOnLeft ? 0u : boundary, 0u, 0u, otherOnLeft ? boundary : width, height, 1u };
    if (box.right > box.left)
    {
        D3D12_TEXTURE_COPY_LOCATION from {}, to {};
        from.pResource = s.color.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = target;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Barrier(cmdList, s.color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyTextureRegion(&to, box.left, 0, 0, &from, &box);
        transitionTarget(currentTargetState);
        Barrier(cmdList, s.color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    DlssNr::Nvfp4::SetComparison(resolvedPreset, "");
}

inline void ReleaseAll()
{
    auto& s = GetState();
    s.host.reset();
    s.answer.Reset();
    s.color.Reset();
    s.device = nullptr;
    s.preset = 0;
    s.reset = true;
}
} // namespace DlssNr::CompareBridge
