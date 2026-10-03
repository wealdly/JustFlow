// R8 luma pyramid step: (w x h) -> (dw x dh) = ceil half, 2x2 mean (edge texels clamped).
Texture2D<float>   src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer C : register(b0) { uint w, h, dw, dh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dw || id.y >= dh) return;
    const int2 hi = int2(w - 1, h - 1), p = int2(id.xy) * 2;
    dst[id.xy] = 0.25 * (src[min(p, hi)] + src[min(p + int2(1, 0), hi)] + src[min(p + int2(0, 1), hi)] + src[min(p + int2(1, 1), hi)]);
}
