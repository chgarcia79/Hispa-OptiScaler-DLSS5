// ResidualAcrossRR v2 -- the MV-reprojected temporal accumulator for the pre-SR NR residual.
//
// Deliberately a SEPARATE shader from dlssnr.hlsl. Regenerating dlssnr.hlsl's blob with a current
// dxc produces materially different DXIL from the committed one (older compiler), and that shader
// carries every NR path -- post-SR, pre-SR, RR, DeferredDLSS, ResidualFG. These two experimental
// modes get their own tiny blob and a second compute PSO instead, so the battle-tested one is
// never touched. The cbuffer and bindings mirror dlssnr.hlsl exactly so DlssNr_Dx12's existing
// root signature and descriptor table are reused as-is; only gResidualBlend is appended, and it
// fits inside DlssNrConstants' existing 256-byte alignment with no size change.
//
//   gMode == 0  Accumulate: (edited - original) blended into the MV-reprojected history layer.
//               history_t = lerp( reproject(history_{t-1}), edited - original, blend )
//               The per-frame ray-trace noise term of (edited - original) is temporally
//               uncorrelated and averages to zero; the enhancement term follows geometry and
//               persists. Invalid reprojection (off-screen / bad MV) -> the
//               history is treated as zero at that pixel and rebuilds over the next frames.
//   gMode == 1  Apply: base + delta * gTransferStrength, clamped non-negative. Run after RR+SR
//               with the upscaled history layer as the delta.

#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;
    float gMvScaleY;
    uint  gGuideWidth;
    uint  gGuideHeight;
    uint  gCompareMode;
    float gCompareSplit;
    float gCompareZoom;
    uint  gCompareSwap;
    uint  gTransfer;
    float gDebugScale;
    uint  gReversibleMode;
    uint  gApplyModel;
    uint  gUseGameExposure;
    float gExposurePreMul;
    uint  gSkinProtection;
    uint  gShowSkinMask;
    float gSkinDetail;
    float gSkinColour;
    float gEnvironmentDetail;
    float gEnvironmentColour;
    float gResidualBlend;   // v2 only: history blend rate, 0..1. 1 == no accumulation (== v1).
    uint gResidualHistoryValid;
    uint gResidualMotionBaseX;
    uint gResidualMotionBaseY;
    float gResidualConfidenceSensitivity;
    uint gHistoryRejection;
    float gHistoryExposureRatio;
    float gHaloSuppression;
    float gHaloBrightSuppression;
    float gHaloRadius;
};

// Same registers and the same SPIR-V binding numbers as dlssnr.hlsl, including the slots these
// modes do not read (gExposure t4, gKeep u1) -- DispatchResidualPass binds a stand-in into them
// exactly as DispatchPass does, and a future Vulkan host path needs the numbering to line up.
#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4>   gSource   : register(t0);  // accumulate: the untouched pre-SR frame. apply: the RR+SR output.
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4>   gModel    : register(t1);  // accumulate: the NR-edited frame. apply: the upscaled delta layer.
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4>   gOriginal : register(t2);  // accumulate: the previous history layer.
#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4>   gMotion   : register(t3);  // raw game motion; active size, offsets and scale come from the host.
#ifndef VK_MODE
Texture2D<float4>   gExposure : register(t4);  // unused here; bound for descriptor-table parity.
#endif
#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
RWTexture2D<float4> gTarget   : register(u0);  // accumulate: the new history layer. apply: the composed frame.
#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
RWTexture2D<float4> gKeep     : register(u1);  // unused here; bound for descriptor-table parity.
#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
SamplerState        gLinear   : register(s0);  // history is sampled at the reprojected coordinate.

float  SanitizeFinite(float v, float fallback)   { return isfinite(v) ? v : fallback; }
float3 SanitizeFinite3(float3 v, float3 fallback)
{
    return float3(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y),
                  SanitizeFinite(v.z, fallback.z));
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kNrEditFloor = 1.0 / 1024.0;

float3 SrgbToLinear(float3 c)
{
    return (c <= 0.04045) ? (c / 12.92) : pow(max((c + 0.055) / 1.055, 0.0), 2.4);
}

float3 LinearToSrgb(float3 c)
{
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float InputLight(int2 pixel)
{
    float3 colour = max(SanitizeFinite3(gSource.Load(int3(pixel, 0)).rgb, 0.0), 0.0);
    float exposure = isfinite(gExposurePreMul) && gExposurePreMul > 1e-6 ? gExposurePreMul : 1.0;
    return log2(max(dot(colour, kLuma) / exposure, kNrEditFloor));
}

float3 NrLogEdit(int2 pixel, out float inputLight)
{
    float3 proxy = max(SanitizeFinite3(SrgbToLinear(gSource.Load(int3(pixel, 0)).rgb), 0.0), 0.0);
    float3 model = max(SanitizeFinite3(SrgbToLinear(gModel.Load(int3(pixel, 0)).rgb), proxy), 0.0);
    inputLight = log2(max(dot(proxy, kLuma), kNrEditFloor));
    return log2(max(model, kNrEditFloor)) - log2(max(proxy, kNrEditFloor));
}

float3 NrFitQualityModel(float3 value)
{
    float y = saturate(dot(value, kLuma));
    float3 d = value - y;
    float amount = 1.0;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 0.0) amount = min(amount, (1.0 - y) / d[c]);
        else if (d[c] < 0.0) amount = min(amount, -y / d[c]);
    }
    return saturate(y + saturate(amount) * d);
}

float4 NrSuppressHalo(int2 pixel)
{
    float4 answer = gModel.Load(int3(pixel, 0));
    if (gHaloSuppression <= 0.0 && gHaloBrightSuppression <= 0.0)
        return answer;
    float inputLight;
    float3 edit = NrLogEdit(pixel, inputLight);
    float a = dot(edit, kLuma);
    float narrow = 0.0, wide = 0.0, narrowWeight = 0.0, wideWeight = 0.0;
    float peak = inputLight, trough = inputLight;
    int radius = int(clamp(round(gHaloRadius), 1.0, 4.0));
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float spatialWeight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            [unroll] for (int band = 0; band < 2; ++band)
            {
                int2 tap = clamp(pixel + int2(x, y) * radius * (band == 0 ? 1 : 3),
                                 int2(0, 0), int2(gWidth - 1, gHeight - 1));
                float light;
                float value = dot(NrLogEdit(tap, light), kLuma);
                peak = max(peak, light);
                trough = min(trough, light);
                float delta = light - inputLight;
                float weight = spatialWeight * exp(-2.0 * delta * delta);
                if (band == 0) { narrow += weight * value; narrowWeight += weight; }
                else { wide += weight * value; wideWeight += weight; }
            }
        }
    }
    narrow /= max(narrowWeight, 1e-6);
    wide /= max(wideWeight, 1e-6);
    float fine = a - narrow, mid = narrow - wide;
    float wmax = smoothstep(1.0, 3.0, peak - inputLight);
    float wmin = smoothstep(1.0, 3.0, inputLight - trough);
    float shapedFine = fine - saturate(gHaloSuppression) * wmax * min(fine, 0.0);
    shapedFine -= saturate(gHaloBrightSuppression) * max(wmax, wmin) * min(shapedFine, 0.0);
    float lift = shapedFine - fine - saturate(gHaloBrightSuppression) * wmin * min(mid, 0.0);
    if (lift <= 1e-6)
        return answer;
    float3 corrected = NrFitQualityModel(max(SrgbToLinear(answer.rgb), 0.0) * exp2(min(lift, 2.0)));
    return float4(LinearToSrgb(corrected), answer.a);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    if (gMode == 0)
    {
        float3 delta = SanitizeFinite3(gModel.Load(int3(id.xy, 0)).rgb -
                                       gSource.Load(int3(id.xy, 0)).rgb, float3(0.0, 0.0, 0.0));

        float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
        uint2 guideSize = uint2(gGuideWidth, gGuideHeight);
        uint2 guidePos = min(uint2(uv * guideSize), guideSize - 1) +
                         uint2(gResidualMotionBaseX, gResidualMotionBaseY);
        float2 motion = gMotion.Load(int3(guidePos, 0)).xy * float2(gMvScaleX, gMvScaleY);
        float2 prevUV = uv + motion;

        bool valid = gResidualHistoryValid != 0 && all(isfinite(motion)) && all(abs(motion) < 2.0) &&
                     all(prevUV >= 0.0) && all(prevUV <= 1.0);

        float4 old = valid ? gOriginal.SampleLevel(gLinear, prevUV, 0) : float4(0.0, 0.0, 0.0, 0.0);
        float3 history = SanitizeFinite3(old.rgb, float3(0.0, 0.0, 0.0));
        float a = clamp(gResidualBlend, 0.0, 1.0);
        float inputLight = 1.0;
        if (gHistoryRejection != 0)
        {
            inputLight = InputLight(int2(id.xy));
            float loI = inputLight, hiI = inputLight;
            float3 lo = delta, hi = delta;
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    int2 q = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(gWidth - 1, gHeight - 1));
                    float3 edit = SanitizeFinite3(gModel.Load(int3(q, 0)).rgb - gSource.Load(int3(q, 0)).rgb, delta);
                    lo = min(lo, edit);
                    hi = max(hi, edit);
                    float light = InputLight(q);
                    loI = min(loI, light);
                    hiI = max(hiI, light);
                }
            }
            valid = valid && all(isfinite(old)) && isfinite(gHistoryExposureRatio) && gHistoryExposureRatio > 0.0;
            history *= valid ? gHistoryExposureRatio : 1.0;
            float changed = valid ? max(max(loI - old.a, old.a - hiI), 0.0) : 1.0;
            a = max(a, smoothstep(0.1, 0.3, changed));
            history = valid ? clamp(history, lo, hi) : delta;
        }
        if (gResidualConfidenceSensitivity > 0.0)
            a = lerp(a, 1.0, saturate(length(delta - history) / gResidualConfidenceSensitivity));
        gTarget[id.xy] = float4(lerp(history, delta, a), inputLight);
        return;
    }

    if (gMode == 3)
    {
        uint srcW = max(gGuideWidth, 1u);
        uint srcH = max(gGuideHeight, 1u);
        int2 srcOff = int2(int(gResidualMotionBaseX), int(gResidualMotionBaseY));
        int2 local = clamp(int2(((float2(id.xy) + 0.5) * float2(srcW, srcH)) / float2(gWidth, gHeight)),
                           int2(0, 0), int2(int(srcW) - 1, int(srcH) - 1));
        gTarget[id.xy] = gSource.Load(int3(srcOff + local, 0));
        return;
    }

    if (gMode == 2)
    {
        gTarget[id.xy] = NrSuppressHalo(int2(id.xy));
        return;
    }

    if (gMode == 1)
    {
        float4 base  = gSource.Load(int3(id.xy, 0));
        float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
        float3 delta = SanitizeFinite3(gModel.SampleLevel(gLinear, uv, 0).rgb, float3(0.0, 0.0, 0.0));

        gTarget[id.xy] = float4(max(base.rgb + delta * gTransferStrength, 0.0), base.a);
        return;
    }

    gTarget[id.xy] = gSource.Load(int3(id.xy, 0));
}
