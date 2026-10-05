// Matched residual compose at output resolution:
//   res = native + (nr_out^ - nr_in^) * strength      (^ = bilinear to w x h; sRGB space as-is)
// then UI rects (blend back to native inside, feathered outside), then the wipe.
// `chroma` scales only the colour part of the model's edit (1 = as the model made it, 0 = keep the
// game's colour exactly and take only its luminance). The model pulls saturation toward photoreal,
// which fights a deliberately stylised palette; the detail and lighting it adds live in the luma.
// Additive, not an OkLab lightness ratio: a ratio from a dark work pixel that lands on a bright native
// pixel (a footprint edge, a wrong warp) blows up (p99 boundary error 88 vs 31, no other gain measured);
// the difference is bounded by the model's own edit.
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
    uint  wipe_mode;
    float wipe_x;
    int   feather;
    uint  nrects;
    uint  w, h, ww, wh;
    uint  strip_w, strip_h;
};
#include "compose_common.hlsli"

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
    res = UiRectsAndWipe(res, nat, q);
    dst[id.xy] = float4(res, 1);
}
