// Decoupled compose: res = native + warp(residual, mv) * strength. The residual was computed on an
// older (model) frame M; mv is the motion from THIS frame to M in work-res pixels, so the content
// now at p was at p + mv in M: sample the residual at uv + mv_uv * warp (bilinear, zero outside
// the frame). Then the same UI rects / feather / wipe as compose.hlsl.
// The warp is only as good as the flow, and the flow is wrong exactly where it matters: at silhouettes,
// on surfaces that were hidden in M, under particles. Adding another surface's edit there is a ghost.
// So the residual's alpha carries M's luma, warped with it, and the edit fades out where that does not
// match this frame's luma (over the same work-pixel footprint): the pixel falls back to native, which is
// never wrong. reject = the luma difference (0..1) at which the edit is half gone; 0 = off.
Texture2D<float4>   native   : register(t0);
Texture2D<float4>   residual : register(t1);   // RGBA16F, ww x wh
Texture2D<float2>   mv       : register(t2);   // R16G16F, any size (sampled by uv)
Texture2D<int>      rects    : register(t3);
RWTexture2D<float4> dst      : register(u0);
SamplerState        samp     : register(s0);
cbuffer C : register(b0)
{
    float strength;
    float chroma;
    uint  wipe_mode;
    float wipe_x;
    int   feather;
    uint  nrects;
    uint  w, h, ww, wh;
    float warp;
    float reject;
    uint  strip_w, strip_h;   // addon mask strip, see compose.hlsl
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
        const float2 suv = uv + mv.SampleLevel(samp, uv, 0) / float2(ww, wh) * warp;
        const float4 rs = residual.SampleLevel(samp, suv, 0);
        float3 r = rs.rgb;
        if (any(suv < 0) || any(suv > 1)) r = 0;
        if (reject > 0)
        {
            const uint s = max(1u, (w + ww / 2) / ww);   // native pixels per work pixel: what M's luma was averaged over
            const uint2 b = (q / s) * s;
            float now = 0;
            [loop] for (uint y = 0; y < s; ++y) [loop] for (uint x = 0; x < s; ++x)
                now += dot(native[min(b + uint2(x, y), uint2(w - 1, h - 1))].rgb, float3(0.2126, 0.7152, 0.0722));
            r *= 1 - smoothstep(0.5 * reject, 1.5 * reject, abs(now / (s * s) - rs.a));
        }
        float3 d = r * strength;
        const float dl = dot(d, float3(0.2126, 0.7152, 0.0722));   // colour part scaled on its own (see compose.hlsl)
        d = lerp(float3(dl, dl, dl), d, chroma);
        res = saturate(nat + d);
    }
    res = UiRectsAndWipe(res, nat, q);
    dst[id.xy] = float4(res, 1);
}
