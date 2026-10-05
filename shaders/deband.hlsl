// Deband - Niklas Haas's algorithm from mpv (deband-pre.glsl, Copyright (c) 2015 Niklas Haas, MIT; see
// NOTICE), written fresh for this pipeline. Streamed and compressed video quantises smooth gradients into
// visible steps, and the sharpen / saturation after this pass would only make them stronger.
// Per iteration i: average four samples at a random angle, `range * i * rand` pixels out, a quarter turn
// apart; take that average wherever it is within threshold / i of the pixel - a real edge differs by more
// and is left alone, a quantisation step does not. Then grain: the output is 8-bit again, and without a
// little noise the smoothed gradient would round straight back into the same bands. UI rects pass through.
Texture2D<float4>   src   : register(t0);
Texture2D<int>      rects : register(t1);
RWTexture2D<float4> dst   : register(u0);
SamplerState        samp  : register(s0);
cbuffer C : register(b0) { uint w, h; float threshold, range, grain; uint iterations, nrects, seed; };

uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
float Rand(inout uint s) { s = Hash(s); return (s >> 8) * (1.0 / 16777216.0); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const int2 p = int2(id.xy);
    float3 res = src[p].rgb;
    [loop] for (uint r = 0; r < min(nrects, 64u); ++r)
        if (p.x >= rects[uint2(r * 4 + 0, 0)] && p.y >= rects[uint2(r * 4 + 1, 0)] &&
            p.x <= rects[uint2(r * 4 + 2, 0)] && p.y <= rects[uint2(r * 4 + 3, 0)])
        { dst[id.xy] = float4(res, 1); return; }
    const float2 px = 1.0 / float2(w, h), uv = (float2(p) + 0.5) * px;
    uint s = Hash(id.x + id.y * 65536u) ^ seed;
    [loop] for (uint i = 1; i <= iterations; ++i)
    {
        const float d = Rand(s) * range * i, ang = Rand(s) * 6.2831853;
        float2 o; sincos(ang, o.y, o.x); o *= d * px;
        const float3 avg = 0.25 * (src.SampleLevel(samp, uv + o, 0).rgb + src.SampleLevel(samp, uv - o, 0).rgb +
                                   src.SampleLevel(samp, uv + float2(-o.y, o.x), 0).rgb + src.SampleLevel(samp, uv + float2(o.y, -o.x), 0).rgb);
        res = lerp(avg, res, step(threshold / i, abs(res - avg)));   // per channel: smooth only what is within the threshold
    }
    res += (Rand(s) - 0.5) * grain;
    dst[id.xy] = float4(saturate(res), 1);
}
