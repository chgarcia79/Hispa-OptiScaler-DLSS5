#pragma once

#include "pch.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <Logger.h>

namespace DlssgParameterSanitizer
{
inline bool TryGetResourceDimensions(ID3D12Resource* tex, unsigned int* outW, unsigned int* outH)
{
    if (!tex || !outW || !outH)
        return false;

    __try
    {
        const D3D12_RESOURCE_DESC desc = tex->GetDesc();
        *outW = static_cast<unsigned int>(desc.Width);
        *outH = desc.Height;
        return (*outW > 0 && *outH > 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *outW = 0;
        *outH = 0;
        return false;
    }
}

inline bool TryGetDimensionParam(NVSDK_NGX_Parameter* params, const char* key, unsigned int& outVal)
{
    if (!params || !key)
        return false;

    unsigned int uVal = 0;
    if (params->Get(key, &uVal) == NVSDK_NGX_Result_Success)
    {
        outVal = uVal;
        return true;
    }

    int iVal = 0;
    if (params->Get(key, &iVal) == NVSDK_NGX_Result_Success && iVal >= 0)
    {
        outVal = static_cast<unsigned int>(iVal);
        return true;
    }

    return false;
}

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
                                    const char* widthKey, const char* heightKey, bool isModdedFg)
{
    if (!params || !tex)
        return;

    unsigned int texW = 0;
    unsigned int texH = 0;
    if (!TryGetResourceDimensions(tex, &texW, &texH))
        return;

    unsigned int subW = 0;
    unsigned int subH = 0;
    const bool hasW = TryGetDimensionParam(params, widthKey, subW);
    const bool hasH = TryGetDimensionParam(params, heightKey, subH);

    if (isModdedFg)
    {
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
    else
    {
        if (hasW && subW > texW)
        {
            params->Set(widthKey, texW);
            LOG_DEBUG("DlssgSanitizer: Clamped overflow {} ({} -> {}) for {}", widthKey, subW, texW, baseKey);
        }

        if (hasH && subH > texH)
        {
            params->Set(heightKey, texH);
            LOG_DEBUG("DlssgSanitizer: Clamped overflow {} ({} -> {}) for {}", heightKey, subH, texH, baseKey);
        }
    }
}

inline void Sanitize(NVSDK_NGX_Parameter* params, bool isCreate, bool isModdedFg = false)
{
    if (!params)
        return;

    if (isCreate)
    {
        unsigned int maxFrames = 0;
        if (TryGetDimensionParam(params, "DLSSG.MultiFrameCountMax", maxFrames) && maxFrames == 0)
        {
            params->Set("DLSSG.MultiFrameCountMax", 1u);
        }

        if (isModdedFg)
        {
            unsigned int w = 0, h = 0, outW = 0, outH = 0;
            if (TryGetDimensionParam(params, "Width", w) &&
                TryGetDimensionParam(params, NVSDK_NGX_Parameter_OutWidth, outW) && outW > w)
            {
                params->Set("Width", outW);
            }
            if (TryGetDimensionParam(params, "Height", h) &&
                TryGetDimensionParam(params, NVSDK_NGX_Parameter_OutHeight, outH) && outH > h)
            {
                params->Set("Height", outH);
            }
        }
        return;
    }

    if (ID3D12Resource* depth = GetResourceParam(params, "DLSSG.Depth"))
    {
        EnsureSubrectDimensions(params, depth, "DLSSG.Depth", "DLSSG.DepthSubrectWidth", "DLSSG.DepthSubrectHeight",
                                isModdedFg);
    }

    if (ID3D12Resource* mvec = GetResourceParam(params, "DLSSG.MVecs"))
    {
        EnsureSubrectDimensions(params, mvec, "DLSSG.MVecs", "DLSSG.MVecsSubrectWidth", "DLSSG.MVecsSubrectHeight",
                                isModdedFg);
    }

    if (ID3D12Resource* hudless = GetResourceParam(params, "DLSSG.HUDLess"))
    {
        EnsureSubrectDimensions(params, hudless, "DLSSG.HUDLess", "DLSSG.HUDLessSubrectWidth",
                                "DLSSG.HUDLessSubrectHeight", isModdedFg);
    }

    if (ID3D12Resource* ui = GetResourceParam(params, "DLSSG.UI"))
    {
        EnsureSubrectDimensions(params, ui, "DLSSG.UI", "DLSSG.UISubrectWidth", "DLSSG.UISubrectHeight", isModdedFg);
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
