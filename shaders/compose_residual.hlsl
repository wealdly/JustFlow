// Decoupled compose: res = native + warp(residual, mv) * strength. The residual was computed on an
// older (model) frame; mv is THIS frame's current -> previous motion in work-res pixels, so the
// content now at p was at p + mv before: sample the residual at uv + mv_uv * warp (bilinear,
// zero outside the frame). Then the same UI rects / feather / wipe as compose.hlsl.
Texture2D<float4>   native   : register(t0);
Texture2D<float4>   residual : register(t1);   // RGBA16F, ww x wh
Texture2D<float2>   mv       : register(t2);   // R16G16F, any size (sampled by uv)
Texture2D<int>      rects    : register(t3);
RWTexture2D<float4> dst      : register(u0);
SamplerState        samp     : register(s0);
cbuffer C : register(b0)
{
    float strength;
    uint  wipe_mode;
    float wipe_x;
    int   feather;
    uint  nrects;
    uint  w, h, ww, wh;
    float warp;
    uint  strip_w, strip_h;   // addon mask strip, see compose.hlsl
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
        const float2 suv = uv + mv.SampleLevel(samp, uv, 0) / float2(ww, wh) * warp;
        float3 r = residual.SampleLevel(samp, suv, 0).rgb;
        if (any(suv < 0) || any(suv > 1)) r = 0;
        res = saturate(nat + r * strength);

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
