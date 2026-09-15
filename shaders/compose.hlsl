// Matched residual compose at output resolution:
//   res = native + (nr_out^ - nr_in^) * strength      (^ = bilinear to w x h; sRGB space as-is,
//   like NeuralScreen kResidualHlsl)
// then UI rects (blend back to native inside, feathered outside), then the wipe.
// `chroma` scales only the colour part of the model's edit (1 = as the model made it, 0 = keep the
// game's colour exactly and take only its luminance). The model pulls saturation toward photoreal,
// which fights a deliberately stylised palette; the detail and lighting it adds live in the luma.
// Rects live in a 256x1 R32_SINT texture (x0,y0,x1,y1 per rect, up to 64): root constants cap at 64 DWORDs.
// Addon mask strip (strip_w x strip_h at the top-left, 0 = off): those output pixels take the
// composed value of the pixel strip_h rows below, so the strip never shows.
Texture2D<float4>   native : register(t0);
Texture2D<float4>   nr_in  : register(t1);
Texture2D<float4>   nr_out : register(t2);
Texture2D<int>      rects  : register(t3);
RWTexture2D<float4> dst    : register(u0);
SamplerState        samp   : register(s0);
cbuffer C : register(b0)
{
    float strength;
    float chroma;
    float saturation;
    uint  wipe_mode;
    float wipe_x;
    int   feather;
    uint  nrects;
    uint  w, h, ww, wh;
    uint  strip_w, strip_h;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    uint2 q = id.xy;   // the pixel composed: itself, or the one strip_h rows below inside the strip
    if (id.y < strip_h && id.x < strip_w) q.y = min(id.y + strip_h, h - 1);
    const float3 nat = native[q].rgb;
    float3 res = nat;
    if (wipe_mode != 2)
    {
        const float2 uv = (float2(q) + 0.5) / float2(w, h);
        const float3 a = nr_in.SampleLevel(samp, uv, 0).rgb;
        const float3 b = nr_out.SampleLevel(samp, uv, 0).rgb;
        float3 d = (b - a) * strength;
        const float dl = dot(d, float3(0.2126, 0.7152, 0.0722));
        d = lerp(float3(dl, dl, dl), d, chroma);
        res = saturate(nat + d);
    }
    {
        // Vibrance is a FILTER, not part of the model: it runs with the neural layer off too, so a
        // model-off profile still gets it. Before the UI rects, so the interface is never saturated.
        const float rl = dot(res, float3(0.2126, 0.7152, 0.0722));
        res = saturate(lerp(float3(rl, rl, rl), res, saturation));
        const float2 p = float2(q);
        const float f = max(feather, 1);
        [loop] for (uint i = 0; i < min(nrects, 64u); ++i)
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
