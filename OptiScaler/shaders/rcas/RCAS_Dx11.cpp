#include "pch.h"

#include "RCAS_Dx11.h"
#include "../Shader_Common.h"

#include "precompile/RCAS_Shader_Dx11.h"
#include "precompile/da_das_sharpen_Shader_Dx11.h"
#include "precompile/da_rcas_sharpen_Shader_Dx11.h"
#include "precompile/baba_neural_sharpen_Shader_Dx11.h"
#include "precompile/opti_sharpen_Shader_Dx11.h"

#include <Config.h>
#include <magic_enum.hpp>

bool RCAS_Dx11::CreateBufferResource(ID3D11Device* InDevice, ID3D11Resource* InResource)
{
    return CreateBufferResourceCommon(InDevice, InResource, _buffer, [](D3D11_TEXTURE2D_DESC& desc)
                                      { desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS; });
}

bool RCAS_Dx11::InitializeViews(ID3D11Texture2D* InResource, ID3D11Texture2D* InMotionVectors,
                                ID3D11Texture2D* OutResource)
{
    auto resultInput = InitializeSRV(InResource, _currentInResource, _srvInput);
    auto resultMvs = InitializeSRV(InMotionVectors, _currentMotionVectors, _srvMotionVectors);
    auto resultOutput = InitializeUAV(OutResource, _currentOutResource, _uavOutput);

    return resultInput && resultMvs && resultOutput;
}

bool RCAS_Dx11::InitializeViewsDA(ID3D11Texture2D* InResource, ID3D11Texture2D* InMotionVectors,
                                  ID3D11Texture2D* InDepth, ID3D11Texture2D* OutResource)
{
    auto resultBase = InitializeViews(InResource, InMotionVectors, OutResource);
    auto resultDepth = InitializeSRV(InDepth, _currentDepth, _srvDepth);

    return resultBase && resultDepth;
}

bool RCAS_Dx11::DispatchRCAS(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, ID3D11Texture2D* InResource,
                             ID3D11Texture2D* InMotionVectors, RcasConstants InConstants, ID3D11Texture2D* OutResource)
{
    (void) InDevice;

    if (!InitializeViews(InResource, InMotionVectors, OutResource))
        return false;

    InternalConstants constants {};

    D3D11_TEXTURE2D_DESC outDesc {};
    OutResource->GetDesc(&outDesc);
    D3D11_TEXTURE2D_DESC mvsDesc {};
    InMotionVectors->GetDesc(&mvsDesc);

    constants.OutputWidth = (uint32_t) outDesc.Width;
    constants.OutputHeight = outDesc.Height;
    constants.MotionWidth = (uint32_t) mvsDesc.Width;
    constants.MotionHeight = mvsDesc.Height;

    FillMotionConstants(constants, InConstants);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    auto hr = InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] Map error {1:x}", _name, hr);

        if (hr == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
            Util::GetDeviceRemovedReason(_device);

        return false;
    }

    memcpy(mappedResource.pData, &constants, sizeof(constants));
    InContext->Unmap(_constantBuffer, 0);

    InContext->CSSetShader(_computeShader, nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, &_constantBuffer);
    InContext->CSSetShaderResources(0, 1, &_srvInput);
    InContext->CSSetShaderResources(1, 1, &_srvMotionVectors);
    InContext->CSSetUnorderedAccessViews(0, 1, &_uavOutput, nullptr);

    UINT dispatchWidth = (constants.OutputWidth + InNumThreadsX - 1) / InNumThreadsX;
    UINT dispatchHeight = (constants.OutputHeight + InNumThreadsY - 1) / InNumThreadsY;

    InContext->Dispatch(dispatchWidth, dispatchHeight, 1);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    InContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    InContext->CSSetShaderResources(0, 2, nullSRV);

    return true;
}

bool RCAS_Dx11::DispatchDepthAdaptive(ID3D11Device* InDevice, ID3D11DeviceContext* InContext,
                                      ID3D11Texture2D* InResource, ID3D11Texture2D* InMotionVectors,
                                      ID3D11Texture2D* InDepth, RcasConstants InConstants, ID3D11Texture2D* OutResource)
{
    (void) InDevice;

    if (InDepth == nullptr || _computeShaderDA == nullptr)
        return false;

    if (!InitializeViewsDA(InResource, InMotionVectors, InDepth, OutResource))
        return false;

    InternalConstantsDA constants {};

    D3D11_TEXTURE2D_DESC outDesc {};
    OutResource->GetDesc(&outDesc);
    D3D11_TEXTURE2D_DESC mvsDesc {};
    InMotionVectors->GetDesc(&mvsDesc);
    D3D11_TEXTURE2D_DESC depthDesc {};
    InDepth->GetDesc(&depthDesc);

    constants.OutputWidth = (uint32_t) outDesc.Width;
    constants.OutputHeight = outDesc.Height;
    constants.MotionWidth = (uint32_t) mvsDesc.Width;
    constants.MotionHeight = mvsDesc.Height;
    constants.DepthWidth = (uint32_t) depthDesc.Width;
    constants.DepthHeight = depthDesc.Height;

    FillMotionConstants(constants, InConstants);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    auto hr = InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] Map error {1:x}", _name, hr);

        if (hr == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
            Util::GetDeviceRemovedReason(_device);

        return false;
    }

    memcpy(mappedResource.pData, &constants, sizeof(constants));
    InContext->Unmap(_constantBuffer, 0);

    InContext->CSSetShader(_computeShaderDA, nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, &_constantBuffer);

    ID3D11ShaderResourceView* srvs[3] = { _srvInput, _srvMotionVectors, _srvDepth };
    InContext->CSSetShaderResources(0, 3, srvs);
    InContext->CSSetUnorderedAccessViews(0, 1, &_uavOutput, nullptr);

    UINT dispatchWidth = (constants.OutputWidth + InNumThreadsX - 1) / InNumThreadsX;
    UINT dispatchHeight = (constants.OutputHeight + InNumThreadsY - 1) / InNumThreadsY;

    InContext->Dispatch(dispatchWidth, dispatchHeight, 1);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    InContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[3] = { nullptr, nullptr, nullptr };
    InContext->CSSetShaderResources(0, 3, nullSRV);

    return true;
}

bool RCAS_Dx11::DispatchDASDepthAdaptive(ID3D11Device* InDevice, ID3D11DeviceContext* InContext,
                                         ID3D11Texture2D* InResource, ID3D11Texture2D* InMotionVectors,
                                         ID3D11Texture2D* InDepth, RcasConstants InConstants,
                                         ID3D11Texture2D* OutResource)
{
    (void) InDevice;

    if (InDepth == nullptr || _computeShaderDASDA == nullptr)
        return false;

    if (!InitializeViewsDA(InResource, InMotionVectors, InDepth, OutResource))
        return false;

    InternalConstantsDA constants {};

    D3D11_TEXTURE2D_DESC outDesc {};
    OutResource->GetDesc(&outDesc);
    D3D11_TEXTURE2D_DESC mvsDesc {};
    InMotionVectors->GetDesc(&mvsDesc);
    D3D11_TEXTURE2D_DESC depthDesc {};
    InDepth->GetDesc(&depthDesc);

    constants.OutputWidth = (uint32_t) outDesc.Width;
    constants.OutputHeight = outDesc.Height;
    constants.MotionWidth = (uint32_t) mvsDesc.Width;
    constants.MotionHeight = mvsDesc.Height;
    constants.DepthWidth = (uint32_t) depthDesc.Width;
    constants.DepthHeight = depthDesc.Height;

    FillMotionConstants(constants, InConstants);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    auto hr = InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] Map error {1:x}", _name, hr);

        if (hr == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
            Util::GetDeviceRemovedReason(_device);

        return false;
    }

    memcpy(mappedResource.pData, &constants, sizeof(constants));
    InContext->Unmap(_constantBuffer, 0);

    InContext->CSSetShader(_computeShaderDASDA, nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, &_constantBuffer);

    ID3D11ShaderResourceView* srvs[3] = { _srvInput, _srvMotionVectors, _srvDepth };
    InContext->CSSetShaderResources(0, 3, srvs);
    InContext->CSSetUnorderedAccessViews(0, 1, &_uavOutput, nullptr);

    UINT dispatchWidth = (constants.OutputWidth + InNumThreadsX - 1) / InNumThreadsX;
    UINT dispatchHeight = (constants.OutputHeight + InNumThreadsY - 1) / InNumThreadsY;

    InContext->Dispatch(dispatchWidth, dispatchHeight, 1);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    InContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[3] = { nullptr, nullptr, nullptr };
    InContext->CSSetShaderResources(0, 3, nullSRV);

    return true;
}

bool RCAS_Dx11::EnsureNeuralShader(SharpenShader InShader, ID3D11Device* InDevice)
{
    const bool isOpti = InShader == SharpenShader::OptiSharpen;
    auto& computeShader = isOpti ? _computeShaderOpti : _computeShaderBaba;
    auto& failed = isOpti ? _optiShaderFailed : _babaShaderFailed;
    if (computeShader != nullptr)
        return true;
    if (failed || InDevice == nullptr)
        return false;

    HRESULT result = isOpti ? CreateComputeShader(InDevice, computeShader, reinterpret_cast<const void*>(opti_sharpen_cso),
                                                  sizeof(opti_sharpen_cso), optiSharpenCode.c_str())
                            : CreateComputeShader(InDevice, computeShader,
                                                  reinterpret_cast<const void*>(baba_neural_sharpen_cso),
                                                  sizeof(baba_neural_sharpen_cso), babaNeuralCode.c_str());
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateComputeShader error for neural shader: {1:X}", _name, result);
        failed = true;
        return false;
    }
    return true;
}

bool RCAS_Dx11::DispatchNeural(SharpenShader InShader, ID3D11Device* InDevice, ID3D11DeviceContext* InContext,
                               ID3D11Texture2D* InResource, ID3D11Texture2D* InMotionVectors, RcasConstants InConstants,
                               ID3D11Texture2D* OutResource)
{
    if (!EnsureNeuralShader(InShader, InDevice))
        return false;
    if (!InitializeViews(InResource, InMotionVectors, OutResource))
        return false;

    D3D11_TEXTURE2D_DESC inDesc {};
    InResource->GetDesc(&inDesc);
    D3D11_TEXTURE2D_DESC outDesc {};
    OutResource->GetDesc(&outDesc);
    D3D11_TEXTURE2D_DESC mvsDesc {};
    InMotionVectors->GetDesc(&mvsDesc);

    InternalConstantsBaba constants {};
    constants.OutputWidth = (int) outDesc.Width;
    constants.OutputHeight = (int) outDesc.Height;
    constants.MotionWidth = (int) mvsDesc.Width;
    constants.MotionHeight = (int) mvsDesc.Height;
    FillMotionConstants(constants, InConstants, InShader, IsFloatFormat(inDesc.Format), (uint32_t) inDesc.Format,
                        magic_enum::enum_name(inDesc.Format));

    D3D11_MAPPED_SUBRESOURCE mappedResource {};
    auto hr = InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr))
        return false;
    memcpy(mappedResource.pData, &constants, sizeof(constants));
    InContext->Unmap(_constantBuffer, 0);

    ID3D11ComputeShader* cs = InShader == SharpenShader::OptiSharpen ? _computeShaderOpti : _computeShaderBaba;
    InContext->CSSetShader(cs, nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, &_constantBuffer);
    ID3D11ShaderResourceView* srvs[2] = { _srvInput, _srvMotionVectors };
    InContext->CSSetShaderResources(0, 2, srvs);
    InContext->CSSetUnorderedAccessViews(0, 1, &_uavOutput, nullptr);
    InContext->Dispatch((outDesc.Width + InNumThreadsX - 1) / InNumThreadsX,
                        (outDesc.Height + InNumThreadsY - 1) / InNumThreadsY, 1);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    InContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    InContext->CSSetShaderResources(0, 2, nullSRV);
    return true;
}

bool RCAS_Dx11::Dispatch(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, ID3D11Texture2D* InResource,
                         ID3D11Texture2D* InMotionVectors, RcasConstants InConstants, ID3D11Texture2D* OutResource,
                         ID3D11Texture2D* InDepth)
{
    if (!_init || InDevice == nullptr || InContext == nullptr || InResource == nullptr || OutResource == nullptr ||
        InMotionVectors == nullptr)
    {
        return false;
    }

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx11 scopedGpuTime(GpuTime.get(), InContext);

    _device = InDevice;

    auto sharpnessShader = Config::Instance()->SharpnessShader.value_or_default();
    if (InDepth == nullptr &&
        (sharpnessShader == SharpenShader::LocalContrastDepthAware || sharpnessShader == SharpenShader::DepthAware))
        sharpnessShader = SharpenShader::RCAS;
    if ((sharpnessShader == SharpenShader::BabaNeural || sharpnessShader == SharpenShader::OptiSharpen) &&
        !EnsureNeuralShader(sharpnessShader, InDevice))
        sharpnessShader = SharpenShader::RCAS;

    SetTimedShader(sharpnessShader);

    if (sharpnessShader == SharpenShader::LocalContrastDepthAware)
    {
        return DispatchDASDepthAdaptive(InDevice, InContext, InResource, InMotionVectors, InDepth, InConstants,
                                        OutResource);
    }
    else if (sharpnessShader == SharpenShader::DepthAware)
    {
        return DispatchDepthAdaptive(InDevice, InContext, InResource, InMotionVectors, InDepth, InConstants,
                                     OutResource);
    }
    else if (sharpnessShader == SharpenShader::BabaNeural || sharpnessShader == SharpenShader::OptiSharpen)
    {
        return DispatchNeural(sharpnessShader, InDevice, InContext, InResource, InMotionVectors, InConstants,
                              OutResource);
    }
    else
    {
        return DispatchRCAS(InDevice, InContext, InResource, InMotionVectors, InConstants, OutResource);
    }
}

RCAS_Dx11::RCAS_Dx11(std::string InName, ID3D11Device* InDevice) : Shader_Dx11(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    HRESULT result = CreateComputeShader(InDevice, _computeShader, reinterpret_cast<const void*>(rcas_cso),
                                         sizeof(rcas_cso), rcasCode.c_str());
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateComputeShader error for rcas shader: {1:X}", _name, result);
        return;
    }

    result = CreateComputeShader(InDevice, _computeShaderDA, reinterpret_cast<const void*>(da_rcas_sharpen_cso),
                                 sizeof(da_rcas_sharpen_cso), daRcasSharpenCode.c_str());
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateComputeShader error for depth aware shader: {1:X}", _name, result);
        return;
    }

    result = CreateComputeShader(InDevice, _computeShaderDASDA, reinterpret_cast<const void*>(da_das_sharpen_cso),
                                 sizeof(da_das_sharpen_cso), dasDASharpenCode.c_str());
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateComputeShader error for DAS depth aware shader: {1:X}", _name, result);
        return;
    }

    // CBV
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = sizeof(InternalConstantsDA);
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    result = InDevice->CreateBuffer(&cbDesc, nullptr, &_constantBuffer);
    if (result != S_OK)
    {
        LOG_ERROR("CreateBuffer error: {0:X}", (UINT) result);
        return;
    }

    _init = true;
}

RCAS_Dx11::~RCAS_Dx11()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_computeShaderDA);
    SAFE_RELEASE(_computeShaderDASDA);
    SAFE_RELEASE(_computeShaderBaba);
    SAFE_RELEASE(_computeShaderOpti);
    SAFE_RELEASE(_srvMotionVectors);
    SAFE_RELEASE(_srvDepth);
    SAFE_RELEASE(_currentMotionVectors);
    SAFE_RELEASE(_currentDepth);
}
