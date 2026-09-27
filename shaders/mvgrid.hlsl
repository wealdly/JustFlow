// The flow field averaged over a 32x18 grid (kMvGridW x kMvGridH in compose.h), for the latewarp engine's mouse model (mouse.cpp): read
// back a few frames later, its median is how far the picture moved. mv: backward flow, work-res px.
Texture2D<float2>   mv  : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer C : register(b0) { uint ww, wh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 32 || id.y >= 18) return;
    const uint x0 = id.x * ww / 32, x1 = (id.x + 1) * ww / 32, y0 = id.y * wh / 18, y1 = (id.y + 1) * wh / 18;
    float2 s = 0; uint n = 0;
    [loop] for (uint y = y0; y < y1; y += 2) [loop] for (uint x = x0; x < x1; x += 2) { s += mv[uint2(x, y)]; ++n; }
    dst[id.xy] = s / max(n, 1u);
}
