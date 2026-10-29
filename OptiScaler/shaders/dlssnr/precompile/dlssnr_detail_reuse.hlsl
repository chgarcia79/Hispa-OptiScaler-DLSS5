// HispaOptiScaler v1.0.4-dev8: Temporal Neural Detail Reuse (N-Frame Cadence 1:2 / 1:3)
// Mode 0 (CaptureDelta): stores log2(edited / base) RGB + normalized depth in gOutCache.
// Mode 1 (ReprojectApply): skips nvngx_dlssnr.dll, reprojects cached detail along MotionVectors,
//                          rejects disocclusions via depth jump & MV silhouette divergence
//                          (gMotionRejectSensitivity 0..1), applies detail (gReuseBlendStrength 0..1),
//                          and propagates ping-pong cache to current frame for multi-step (1:3) tracking.

cbuffer DetailReuseParams : register(b0)
{
    uint  gMode;
    uint  gWidth;
    uint  gHeight;
    uint  gMotionWidth;
    uint  gMotionHeight;
    uint  gDepthWidth;
    uint  gDepthHeight;
    float gMvScaleUvX;
    float gMvScaleUvY;
    float gReuseBlendStrength;
    float gMotionRejectSensitivity;
    uint  gDepthInverted;
};

Texture2D<float4>   gBaseColor     : register(t0);
Texture2D<float4>   gEditedOrCache : register(t1);
Texture2D<float4>   gMotion        : register(t2);
Texture2D<float>    gDepth         : register(t3);

RWTexture2D<float4> gOutCache      : register(u0);
RWTexture2D<float4> gOutColor      : register(u1);

SamplerState        gLinearClamp   : register(s0);

static const float kLogFloor = 1e-4f;

float Sanitize1(float v, float fb)
{
    return isfinite(v) ? v : fb;
}

float3 Sanitize3(float3 v, float3 fb)
{
    return float3(Sanitize1(v.x, fb.x), Sanitize1(v.y, fb.y), Sanitize1(v.z, fb.z));
}

float SampleNormalizedDepth(float2 uv)
{
    uint dw = max(gDepthWidth, 1u);
    uint dh = max(gDepthHeight, 1u);
    int2 dp = clamp(int2(uv * float2(dw, dh)), int2(0, 0), int2(int(dw) - 1, int(dh) - 1));
    float d = saturate(Sanitize1(gDepth.Load(int3(dp, 0)).r, 0.0f));
    return (gDepthInverted != 0u) ? (1.0f - d) : d;
}

float2 LoadMotionUv(int2 mvPos)
{
    int2 clamped = clamp(mvPos, int2(0, 0), int2(int(max(gMotionWidth, 1u)) - 1, int(max(gMotionHeight, 1u)) - 1));
    float2 raw = gMotion.Load(int3(clamped, 0)).xy;
    return float2(Sanitize1(raw.x, 0.0f), Sanitize1(raw.y, 0.0f)) * float2(gMvScaleUvX, gMvScaleUvY);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gWidth || dtid.y >= gHeight)
        return;

    int2 px = int2(dtid.xy);
    float2 dims = float2(max(gWidth, 1u), max(gHeight, 1u));
    float2 uv = (float2(dtid.xy) + 0.5f) / dims;
    float curDepth = SampleNormalizedDepth(uv);

    if (gMode == 0u)
    {
        float3 baseRgb = max(Sanitize3(gBaseColor.Load(int3(px, 0)).rgb, 0.0f.xxx), 0.0f.xxx);
        float3 editRgb = max(Sanitize3(gEditedOrCache.Load(int3(px, 0)).rgb, baseRgb), 0.0f.xxx);
        float3 logDelta = clamp(log2(max(editRgb, kLogFloor)) - log2(max(baseRgb, kLogFloor)), -2.0f, 2.0f);
        gOutCache[dtid.xy] = float4(logDelta, curDepth);
        return;
    }

    float4 base4 = gBaseColor.Load(int3(px, 0));
    float3 baseRgb = max(Sanitize3(base4.rgb, 0.0f.xxx), 0.0f.xxx);

    int2 mvPos = int2(uv * float2(max(gMotionWidth, 1u), max(gMotionHeight, 1u)));
    float2 mvUv = LoadMotionUv(mvPos);
    float2 prevUv = uv + mvUv;

    bool validUv = all(abs(mvUv) < 1.5f) && all(prevUv >= 0.0f) && all(prevUv <= 1.0f);
    if (!validUv)
    {
        gOutCache[dtid.xy] = float4(0.0f, 0.0f, 0.0f, curDepth);
        gOutColor[dtid.xy] = float4(baseRgb, base4.a);
        return;
    }

    float4 cached = gEditedOrCache.SampleLevel(gLinearClamp, prevUv, 0.0f);
    float3 logDelta = clamp(Sanitize3(cached.rgb, 0.0f.xxx), -2.0f, 2.0f);
    float prevDepth = saturate(Sanitize1(cached.a, curDepth));

    float confidence = 1.0f;
    float rejectSens = saturate(gMotionRejectSensitivity);
    if (rejectSens > 1e-4f)
    {
        float depthJump = abs(curDepth - prevDepth) / max(max(curDepth, prevDepth), 1e-3f);
        float2 mvR = LoadMotionUv(mvPos + int2(1, 0));
        float2 mvD = LoadMotionUv(mvPos + int2(0, 1));
        float mvEdgePx = length((mvUv - mvR) * dims) + length((mvUv - mvD) * dims);
        float speedPx = length(mvUv * dims);
        float penalty = (depthJump * 10.0f + mvEdgePx * 0.30f + speedPx * 0.012f) * (0.25f + 2.25f * rejectSens);
        confidence = saturate(1.0f - penalty);
    }

    float3 weightedDelta = logDelta * (saturate(gReuseBlendStrength) * confidence);
    float3 composedRgb = max(baseRgb * exp2(clamp(weightedDelta, -2.0f, 2.0f)), 0.0f.xxx);

    gOutCache[dtid.xy] = float4(logDelta * confidence, curDepth);
    gOutColor[dtid.xy] = float4(composedRgb, base4.a);
}
