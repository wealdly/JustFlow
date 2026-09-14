// BGRA8 capture -> RGBA8. A B8G8R8A8_UNORM SRV already returns .rgb in RGB order, so this is a copy.
Texture2D<float4>   src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w, h; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    dst[id.xy] = src[id.xy];
}
