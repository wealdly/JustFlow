// 3x3 vector median of a flow grid: the neighbour with the smallest summed L1 distance to the other
// eight. Unlike a per-component median it only ever outputs a vector that was actually measured, so
// it removes isolated mismatches without inventing motion between two objects.
Texture2D<float2>   src : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer C : register(b0) { uint gw, gh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gw || id.y >= gh) return;
    float2 v[9];
    [unroll] for (int i = 0; i < 9; ++i)
        v[i] = src[clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(gw - 1, gh - 1))];
    float best_d = 1e30; float2 best = v[4];
    [unroll] for (int a = 0; a < 9; ++a)
    {
        float d = 0;
        [unroll] for (int b = 0; b < 9; ++b) { const float2 e = abs(v[a] - v[b]); d += e.x + e.y; }
        if (d < best_d) { best_d = d; best = v[a]; }
    }
    dst[id.xy] = best;
}
