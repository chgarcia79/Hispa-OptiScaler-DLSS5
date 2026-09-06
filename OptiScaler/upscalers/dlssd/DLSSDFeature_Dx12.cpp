#include <pch.h>
#include "DLSSDFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>
#include <State.h>
#include <vector>

bool DLSSDFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSSD(InCommandList, InParameters);
}

bool DLSSDFeatureDx12::InitDLSSD(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssdInitedDx12)
    {
        _dlssdInitedDx12 = NVNGXProxy::InitDx12(Device);

        if (!_dlssdInitedDx12)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            (NVNGXProxy::D3D12_Shutdown() != nullptr || NVNGXProxy::D3D12_Shutdown1() != nullptr) &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // delay between init and create feature
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSSD feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        ProcessInitParams(InParameters);

        _p_dlssdHandle = &_dlssdHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_RayReconstruction,
                                                         InParameters, &_p_dlssdHandle);
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
        else
        {
            LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success, HandleId: {0}", _p_dlssdHandle->Id);
        }
    }
    else
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    if (NVNGXProxy::D3D12_EvaluateFeature() == nullptr)
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    ProcessEvaluateParams(InParameters);

    ID3D12Resource* paramColor = nullptr;
    ID3D12Resource* paramDepth = nullptr;
    ID3D12Resource* paramMotion = nullptr;
    ID3D12Resource* paramExposure = nullptr;
    ID3D12Resource* paramBiasMask = nullptr;
    ID3D12Resource* paramOutput = nullptr;

    if (InParameters->Get(NVSDK_NGX_Parameter_Color, &paramColor) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_Color, (void**) &paramColor);

    if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

    if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMotion) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramMotion);

    if (InParameters->Get(NVSDK_NGX_Parameter_ExposureTexture, &paramExposure) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_ExposureTexture, (void**) &paramExposure);

    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &paramBiasMask) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void**) &paramBiasMask);

    if (InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);

    std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> restoredBarriers;
    auto cmdType = InCommandList->GetType();
    bool isPureDark = Config::Instance()->PureDarkBridge.value_or(false);

    auto transitionInput = [&](ID3D12Resource* res, std::optional<int32_t> configBarrier, D3D12_RESOURCE_STATES defaultState, const char* name)
    {
        if (res == nullptr || res == paramOutput)
            return;

        for (const auto& b : restoredBarriers)
        {
            if (b.first == res)
                return;
        }

        D3D12_RESOURCE_STATES arrivalState = D3D12_RESOURCE_STATE_COMMON;
        if (!GetArrivalResourceState(res, configBarrier, defaultState, arrivalState))
            return;

        if (cmdType == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            if (arrivalState & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
                return;
        }

        if (arrivalState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            ResourceBarrier(InCommandList, res, arrivalState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            restoredBarriers.push_back({ res, arrivalState });
            LOG_INFO("DLSSDFeatureDx12::EvaluateInternal transitioned {} ({:p}) from 0x{:X} to NON_PIXEL_SHADER_RESOURCE (0x{:X})",
                     name, (void*) res, (uint32_t) arrivalState, (uint32_t) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    };

    transitionInput(paramColor, Config::Instance()->ColorResourceBarrier.has_value() ? Config::Instance()->ColorResourceBarrier.value() : std::optional<int32_t>{},
                    isPureDark ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON, "Color");
    transitionInput(paramDepth, Config::Instance()->DepthResourceBarrier.has_value() ? Config::Instance()->DepthResourceBarrier.value() : std::optional<int32_t>{},
                    isPureDark ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON, "Depth");
    transitionInput(paramMotion, Config::Instance()->MVResourceBarrier.has_value() ? Config::Instance()->MVResourceBarrier.value() : std::optional<int32_t>{},
                    isPureDark ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON, "MotionVectors");
    transitionInput(paramExposure, Config::Instance()->ExposureResourceBarrier.has_value() ? Config::Instance()->ExposureResourceBarrier.value() : std::optional<int32_t>{},
                    D3D12_RESOURCE_STATE_COMMON, "Exposure");
    transitionInput(paramBiasMask, Config::Instance()->MaskResourceBarrier.has_value() ? Config::Instance()->MaskResourceBarrier.value() : std::optional<int32_t>{},
                    D3D12_RESOURCE_STATE_COMMON, "BiasMask");

    NVSDK_NGX_Result nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssdHandle, InParameters, NULL);

    for (auto it = restoredBarriers.rbegin(); it != restoredBarriers.rend(); ++it)
    {
        ResourceBarrier(InCommandList, it->first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, it->second);
        LOG_INFO("DLSSDFeatureDx12::EvaluateInternal restored res {:p} to 0x{:X}", (void*) it->first, (uint32_t) it->second);
    }

    if (nvResult != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
        return false;
    }

    _frameCount++;

    return true;
}

DLSSDFeatureDx12::DLSSDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters),
      DLSSDFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSDFeatureDx12::~DLSSDFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_dlssdHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssdHandle);
}
