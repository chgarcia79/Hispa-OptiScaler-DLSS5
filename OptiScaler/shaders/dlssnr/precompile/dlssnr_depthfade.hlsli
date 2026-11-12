#ifndef DLSSNR_DEPTHFADE_HLSLI
#define DLSSNR_DEPTHFADE_HLSLI

// Atenuacion espacial por profundidad (Depth Fade) para DLSS 5 Neural Rendering.
// Evalua una curva suave (smoothstep) entre NearDepth y FarDepth y separa la zona de cielo.

#ifdef VK_MODE

float ComputeDepthFadeWeightRaw(float2 uv) { return 1.0; }
float ComputeDepthFadeWeight(float2 uv) { return 1.0; }

#else

float ComputeDepthFadeWeightRaw(float2 uv)
{
    if (gHasValidDepth == 0)
        return 1.0;

    uint w = gDepthWidth;
    uint h = gDepthHeight;
    if (w == 0 || h == 0)
        gDepth.GetDimensions(w, h);
    if (w == 0 || h == 0)
        return 1.0;

    int2 texCoord = clamp(int2(uv * float2(w, h)), int2(0, 0), int2((int) w - 1, (int) h - 1));
    float rawZ = gDepth.Load(int3(texCoord, 0)).r;

    // zInv = 1.0 cerca de camara, -> 0.0 en infinito/cielo para Reverse-Z o Normal-Z.
    float zInv = gDepthReverseZ != 0 ? saturate(rawZ) : saturate(1.0 - rawZ);
    float dist = 0.02 / (0.02 + zInv);

    float nearD = clamp(gDepthNear, 0.0001, 0.9999);
    float farD = max(clamp(gDepthFar, 0.0001, 1.0), nearD + 1e-4);

    float t = smoothstep(nearD, farD, dist);
    float surfaceWeight = lerp(saturate(gDepthNearStrength), saturate(gDepthFarStrength), t);
    float skyMask = 1.0 - smoothstep(0.0, 2e-4, zInv);

    return lerp(surfaceWeight, saturate(gDepthSkyStrength), skyMask);
}

float ComputeDepthFadeWeight(float2 uv)
{
    if (gDepthFadeEnabled == 0 || gHasValidDepth == 0)
        return 1.0;
    return ComputeDepthFadeWeightRaw(uv);
}

#endif
#endif // DLSSNR_DEPTHFADE_HLSLI
