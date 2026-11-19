#pragma once

#include "SysUtils.h"
#include "OptiTypes.h"

#include "shaders/BABA_NEURAL_Shader.h"
#include "shaders/DA_DAS_Shader.h"
#include "shaders/DA_RCAS_Shader.h"
#include "shaders/OPTI_SHARPEN_Shader.h"
#include "shaders/RCAS_Shader.h"

#include <atomic>
#include <dxgiformat.h>
#include <string_view>

struct RcasConstants
{
    float Sharpness;

    float MvScaleX;
    float MvScaleY;

    float CameraNear;
    float CameraFar;

    bool DepthIsLinear;
    bool DepthIsReversed;

    bool IsHdr;
};

class RCAS_Common
{
  protected:
    struct alignas(256) InternalConstants
    {
        float Sharpness;
        float Contrast;

        // Motion Vector Stuff
        int DynamicSharpenEnabled;
        int DisplaySizeMV;
        int Debug;
        int MotionWidth;
        int MotionHeight;

        float MotionSharpness;
        float MotionTextureScale;
        float MvScaleX;
        float MvScaleY;
        float Threshold;
        float ScaleLimit;

        int OutputWidth;
        int OutputHeight;
    };

    struct alignas(256) InternalConstantsDA
    {
        float Sharpness;

        int DepthIsLinear;
        int DepthIsReversed;

        float DepthScale;
        float DepthBias;

        float DepthLinearA;
        float DepthLinearB;
        float DepthLinearC;

        int DynamicSharpenEnabled;
        int DisplaySizeMV;
        int Debug;

        float MotionSharpness;
        float MotionTextureScale;
        float MvScaleX;
        float MvScaleY;
        float MotionThreshold;
        float MotionScaleLimit;

        float DepthTextureScale;

        int ClampOutput;

        int OutputWidth;
        int OutputHeight;
        int MotionWidth;
        int MotionHeight;
        int DepthWidth;
        int DepthHeight;
    };

    struct alignas(256) InternalConstantsBaba
    {
        float Sharpness;
        float EdgeResponse;
        int Model;
        int ViewMode;

        int DynamicSharpenEnabled;
        int DisplaySizeMV;
        int Debug;
        int MotionWidth;
        int MotionHeight;

        float MotionSharpness;
        float MotionTextureScale;
        float MvScaleX;
        float MvScaleY;
        float Threshold;
        float ScaleLimit;

        int OutputWidth;
        int OutputHeight;

        int ClampOutput;
        int IsHdr;
    };

    static bool IsFloatFormat(DXGI_FORMAT InFormat);
    void SetTimedShader(SharpenShader InShader);
    void FillMotionConstants(InternalConstants& OutConstants, const RcasConstants& InConstants);
    void FillMotionConstants(InternalConstantsDA& OutConstants, const RcasConstants& InConstants);
    void FillMotionConstants(InternalConstantsBaba& OutConstants, const RcasConstants& InConstants,
                             SharpenShader InShader, bool InputIsFloat, uint32_t InputFormat,
                             std::string_view FormatName);

  private:
    std::atomic<const char*> _timedShaderName { "RCAS" };
    int64_t _babaLoggedFormat = -1;
    bool _babaLoggedLinear = false;
    bool _babaLoggedHdr = false;

  public:
    const char* TimedShaderName() const { return _timedShaderName.load(std::memory_order_relaxed); }
};
