// DenoiseFirst 1:1 Ratio Transfer Shader (CS 5.0)
// Extracts per-pixel neural ratio (NR_Output / Clean_1to1) on the un-jittered 1:1 grid,
// resamples at the current sub-pixel jitter offset (gJitterX, gJitterY) via 4x4 Catmull-Rom
// with 2x2 neighborhood anti-ringing clamp, anti-NaN floor (1e-4) + [gMinRatio, gMaxRatio] clamp,
// and firefly suppression before multiplying onto the raw noisy Path Tracing / HDR input.

cbuffer DenoiseFirstConstants : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float gJitterX;
    float gJitterY;
    float gStrength;
    float gMinRatio;
    float gMaxRatio;
    uint  gFireflyGuard;
};

Texture2D<float4>   gRawColor  : register(t0);
Texture2D<float4>   gNrEdited  : register(t1);
Texture2D<float4>   gClean1to1 : register(t2);
RWTexture2D<float4> gOutput    : register(u0);

static const float kEps = 1e-4f;

float Finite1(float v, float fallback)
{
    return isfinite(v) ? v : fallback;
}

float3 Finite3(float3 v, float3 fallback)
{
    return float3(Finite1(v.x, fallback.x), Finite1(v.y, fallback.y), Finite1(v.z, fallback.z));
}

float CatmullRom(float x)
{
    x = abs(x);
    if (x < 1.0f)
        return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    if (x < 2.0f)
        return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    return 0.0f;
}

float3 RatioAt(int2 p)
{
    p = clamp(p, int2(0, 0), int2(gWidth, gHeight) - 1);
    const float3 nrVal    = max(Finite3(gNrEdited.Load(int3(p, 0)).rgb, float3(0.0f, 0.0f, 0.0f)), 0.0f);
    const float3 cleanVal = max(Finite3(gClean1to1.Load(int3(p, 0)).rgb, float3(0.0f, 0.0f, 0.0f)), kEps);
    const float3 safeNum  = (nrVal < kEps && cleanVal <= kEps) ? cleanVal : nrVal;
    const float3 ratio    = safeNum / max(cleanVal, kEps);
    return clamp(Finite3(ratio, float3(1.0f, 1.0f, 1.0f)), gMinRatio, gMaxRatio);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const float4 raw = gRawColor.Load(int3(id.xy, 0));
    const float3 rawRgb = max(Finite3(raw.rgb, float3(0.0f, 0.0f, 0.0f)), 0.0f);

    const float2 shift  = float2(Finite1(gJitterX, 0.0f), Finite1(gJitterY, 0.0f));
    const float2 sample = float2(id.xy) + 0.5f + shift;
    const float2 base   = floor(sample - 0.5f);
    const float2 frac   = sample - 0.5f - base;

    float3 accum = float3(0.0f, 0.0f, 0.0f);
    float  wSum  = 0.0f;
    float3 lo    = float3(1e30f, 1e30f, 1e30f);
    float3 hi    = float3(-1e30f, -1e30f, -1e30f);

    [unroll]
    for (int ky = -1; ky <= 2; ++ky)
    {
        const float wy = CatmullRom(frac.y - ky);
        [unroll]
        for (int kx = -1; kx <= 2; ++kx)
        {
            const float  w   = wy * CatmullRom(frac.x - kx);
            const float3 tap = RatioAt(int2(base) + int2(kx, ky));
            accum += tap * w;
            wSum  += w;
            if (kx >= 0 && kx <= 1 && ky >= 0 && ky <= 1)
            {
                lo = min(lo, tap);
                hi = max(hi, tap);
            }
        }
    }

    float3 ratio = (abs(wSum) > 1e-6f) ? (accum / wSum) : RatioAt(int2(id.xy));
    ratio = clamp(Finite3(ratio, float3(1.0f, 1.0f, 1.0f)), max(lo, gMinRatio), min(hi, gMaxRatio));

    // Path-tracing firefly guard: avoid multiplying 1-spp specular fireflies above clean luminance
    const float3 cleanCenter = max(Finite3(gClean1to1.Load(int3(id.xy, 0)).rgb, float3(0.0f, 0.0f, 0.0f)), 0.0f);
    const float  cleanLum    = dot(cleanCenter, float3(0.2126f, 0.7152f, 0.0722f));
    const float  rawLum      = dot(rawRgb, float3(0.2126f, 0.7152f, 0.0722f));
    const float  ordinary    = (gFireflyGuard != 0u) ? saturate(max(cleanLum, 0.0f) / max(rawLum, kEps)) : 1.0f;

    const float3 gain   = max(lerp(float3(1.0f, 1.0f, 1.0f), ratio, clamp(gStrength, 0.0f, 2.0f)), 0.0f);
    const float3 factor = max(1.0f + (gain - 1.0f) * ordinary, 0.0f);
    const float3 outRgb = max(Finite3(rawRgb * factor, rawRgb), 0.0f);

    gOutput[id.xy] = float4(outRgb, raw.a);
}
