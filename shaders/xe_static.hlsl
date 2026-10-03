// Per-pair static test for xe_interp, at L0 (the flow's luma, native / factor). For each pixel of cur:
//   e0 = luma disagreement of prev and cur at the SAME place  (the zero-motion hypothesis)
//   ef = luma disagreement of prev at x + bwd(x) and cur at x (the flow's claim on the pair it came from)
// each summed over a +-1 L0 px cross (+-2 native at factor 2: a text stroke). Static UI has e0 ~ 0 by
// construction; moving content does not. Once per real frame instead of 10 samples per pixel per
// present (which cost 1.3 ms of every 1.6 ms interpolation pass); xe_interp turns (e0, ef) into a weight.
Texture2D<float>    cur  : register(t0);
Texture2D<float>    prev : register(t1);
Texture2D<float2>   bwd  : register(t2);   // L0 grid, one vector per 8x8 block, L0 pixels
RWTexture2D<float2> dst  : register(u0);   // (e0, ef), L0 size
SamplerState        lin  : register(s0);
cbuffer C : register(b0) { uint lw, lh, gw, gh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= lw || id.y >= lh) return;
    const float2 x = float2(id.xy) + 0.5, size = float2(lw, lh);
    const float2 b = bwd.SampleLevel(lin, x / (8.0 * float2(gw, gh)), 0);
    const float2 cross[5] = { float2(0, 0), float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1) };
    float e0 = 0, ef = 0;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        const float2 p = x + cross[i];
        const float c = cur.SampleLevel(lin, p / size, 0);
        e0 += abs(prev.SampleLevel(lin, p / size, 0) - c);
        ef += abs(prev.SampleLevel(lin, (p + b) / size, 0) - c);
    }
    dst[id.xy] = float2(e0, ef);
}
