// RGBA8 (w x h) -> R8 luma (gw x gh): exact area average of the bw x bh block of Rec.709 luma.
Texture2D<float4>  src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer C : register(b0) { uint w, h, gw, gh, bw, bh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gw || id.y >= gh) return;
    const uint x0 = id.x * bw, y0 = id.y * bh;
    const uint x1 = min(x0 + bw, w), y1 = min(y0 + bh, h);
    float sum = 0;
    [loop] for (uint y = y0; y < y1; ++y)
        [loop] for (uint x = x0; x < x1; ++x)
            sum += dot(src[uint2(x, y)].rgb, float3(0.2126, 0.7152, 0.0722));
    const uint n = (x1 - x0) * (y1 - y0);
    dst[id.xy] = n ? sum / n : 0;
}
