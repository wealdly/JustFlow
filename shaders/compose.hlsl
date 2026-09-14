// Matched residual compose at output resolution:
//   res = native + (nr_out^ - nr_in^) * strength      (^ = bilinear to w x h; sRGB space as-is,
//   like NeuralScreen kResidualHlsl)
// then UI rects (blend back to native inside, feathered outside), then the wipe.
// Rects live in a 64x1 R32_SINT texture (x0,y0,x1,y1 per rect): root constants cap at 64 DWORDs.
Texture2D<float4>   native : register(t0);
Texture2D<float4>   nr_in  : register(t1);
Texture2D<float4>   nr_out : register(t2);
Texture2D<int>      rects  : register(t3);
RWTexture2D<float4> dst    : register(u0);
SamplerState        samp   : register(s0);
cbuffer C : register(b0)
{
    float strength;
    uint  wipe_mode;
    float wipe_x;
    int   feather;
    uint  nrects;
    uint  w, h, ww, wh;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float3 nat = native[id.xy].rgb;
    float3 res = nat;
    if (wipe_mode != 2)
    {
        const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
        const float3 a = nr_in.SampleLevel(samp, uv, 0).rgb;
        const float3 b = nr_out.SampleLevel(samp, uv, 0).rgb;
        res = saturate(nat + (b - a) * strength);

        const float2 p = float2(id.xy);
        const float f = max(feather, 1);
        [loop] for (uint i = 0; i < min(nrects, 16u); ++i)
        {
            const float x0 = rects[uint2(i * 4 + 0, 0)], y0 = rects[uint2(i * 4 + 1, 0)];
            const float x1 = rects[uint2(i * 4 + 2, 0)], y1 = rects[uint2(i * 4 + 3, 0)];
            const float d = max(max(max(x0 - p.x, p.x - x1), max(y0 - p.y, p.y - y1)), 0);
            res = lerp(res, nat, 1 - smoothstep(0, f, d));
        }
        if (wipe_mode == 1)
        {
            const float split = wipe_x * w;
            if (abs(p.x - split) < 1.5) res = 1;
            else if (p.x < split)      res = nat;
        }
    }
    dst[id.xy] = float4(res, 1);
}
