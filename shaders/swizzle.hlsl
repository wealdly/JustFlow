// The capture -> RGBA8: a copy for BGRA8, the HDR conversion for FP16 (capture_in.hlsli).
#include "capture_in.hlsli"
Texture2D<float4>   src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w, h; float white; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    dst[id.xy] = CaptureIn(src, id.xy, white);
}
