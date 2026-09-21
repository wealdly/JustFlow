// RGBA8 (w x h) -> RGBA8 (dw x dh): exact area box filter over the fractional source footprint
// (partial pixels weighted by coverage). NeuralScreen quality_shaders.h kScaleHlsl4 area branch.
Texture2D<float4>   src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w, h, dw, dh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dw || id.y >= dh) return;
    const int2 hi = int2(w - 1, h - 1);
    const float2 scale = float2(w, h) / float2(dw, dh);
    const float2 lo = float2(id.xy) * scale;
    const float2 end = float2(id.xy + 1) * scale;
    float4 sum = 0;
    [loop] for (int y = int(floor(lo.y)); y < int(ceil(end.y)); ++y)
    {
        const float wy = max(0, min(end.y, float(y + 1)) - max(lo.y, float(y)));
        [loop] for (int x = int(floor(lo.x)); x < int(ceil(end.x)); ++x)
        {
            const float wx = max(0, min(end.x, float(x + 1)) - max(lo.x, float(x)));
            sum += src[clamp(int2(x, y), int2(0, 0), hi)] * wx * wy;
        }
    }
    dst[id.xy] = sum / ((end.x - lo.x) * (end.y - lo.y));
}
