#pragma once

#include "pch.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <Logger.h>

namespace DlssgParameterSanitizer
{
inline ID3D12Resource* GetResourceParam(NVSDK_NGX_Parameter* params, const char* key)
{
    if (!params || !key)
        return nullptr;

    ID3D12Resource* res = nullptr;
    if (params->Get(key, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    void* raw = nullptr;
    if (params->Get(key, &raw) == NVSDK_NGX_Result_Success && raw != nullptr)
        return static_cast<ID3D12Resource*>(raw);

    return nullptr;
}

inline void EnsureSubrectDimensions(NVSDK_NGX_Parameter* params, ID3D12Resource* tex, const char* baseKey,
                                    const char* widthKey, const char* heightKey)
{
    if (!params || !tex)
        return;

    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    const unsigned int texW = static_cast<unsigned int>(desc.Width);
    const unsigned int texH = desc.Height;

    unsigned int subW = 0;
    unsigned int subH = 0;
    const bool hasW = params->Get(widthKey, &subW) == NVSDK_NGX_Result_Success;
    const bool hasH = params->Get(heightKey, &subH) == NVSDK_NGX_Result_Success;

    if (!hasW || subW == 0 || subW > texW)
    {
        params->Set(widthKey, texW);
        LOG_DEBUG("DlssgSanitizer: Normalized {} ({} -> {}) for {}", widthKey, subW, texW, baseKey);
    }

    if (!hasH || subH == 0 || subH > texH)
    {
        params->Set(heightKey, texH);
        LOG_DEBUG("DlssgSanitizer: Normalized {} ({} -> {}) for {}", heightKey, subH, texH, baseKey);
    }
}

inline void Sanitize(NVSDK_NGX_Parameter* params, bool isCreate)
{
    if (!params)
        return;

    if (ID3D12Resource* depth = GetResourceParam(params, "DLSSG.Depth"))
    {
        EnsureSubrectDimensions(params, depth, "DLSSG.Depth", "DLSSG.DepthSubrectWidth", "DLSSG.DepthSubrectHeight");
    }

    if (ID3D12Resource* mvec = GetResourceParam(params, "DLSSG.MVecs"))
    {
        EnsureSubrectDimensions(params, mvec, "DLSSG.MVecs", "DLSSG.MVecsSubrectWidth", "DLSSG.MVecsSubrectHeight");
    }

    if (ID3D12Resource* hudless = GetResourceParam(params, "DLSSG.HUDLess"))
    {
        EnsureSubrectDimensions(params, hudless, "DLSSG.HUDLess", "DLSSG.HUDLessSubrectWidth",
                                "DLSSG.HUDLessSubrectHeight");
    }

    if (ID3D12Resource* ui = GetResourceParam(params, "DLSSG.UI"))
    {
        EnsureSubrectDimensions(params, ui, "DLSSG.UI", "DLSSG.UISubrectWidth", "DLSSG.UISubrectHeight");
    }

    if (isCreate)
    {
        unsigned int maxFrames = 0;
        if (params->Get("DLSSG.MultiFrameCountMax", &maxFrames) == NVSDK_NGX_Result_Success && maxFrames == 0)
        {
            params->Set("DLSSG.MultiFrameCountMax", 1u);
        }
    }
}

inline void LogEvaluateParameters(NVSDK_NGX_Parameter* params)
{
    if (!params)
        return;

    static uint32_t loggedFrames = 0;
    if (loggedFrames >= 3)
        return;

    loggedFrames++;

    ID3D12Resource* backbuffer = GetResourceParam(params, "DLSSG.Backbuffer");
    ID3D12Resource* depth = GetResourceParam(params, "DLSSG.Depth");
    ID3D12Resource* mvec = GetResourceParam(params, "DLSSG.MVecs");
    ID3D12Resource* hudless = GetResourceParam(params, "DLSSG.HUDLess");
    ID3D12Resource* ui = GetResourceParam(params, "DLSSG.UI");

    int multiFrameCount = 0;
    params->Get("DLSSG.MultiFrameCount", &multiFrameCount);

    LOG_DEBUG("DlssgSanitizer [Frame {}]: MultiFrameCount={}, BB={:p}, Depth={:p}, MVecs={:p}, HUDLess={:p}, UI={:p}",
              loggedFrames, multiFrameCount, (void*) backbuffer, (void*) depth, (void*) mvec, (void*) hudless,
              (void*) ui);
}
} // namespace DlssgParameterSanitizer
