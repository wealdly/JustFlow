// engine=video: the frame at time t (0..1) between two real frames A and B, for 2D video timed by its own
// clock (fg.cpp VideoPresent). mv is the pipeline's backward flow of B (where B's content was in A, work
// pixels), so it belongs to B's pixels: the content shown at output pixel p sits in B at q with
// q + (1-t)*f(q) = p, found by a few fixed-point steps - reading the flow at p itself took it from
// whatever object was there and dragged thin foreground (poles) along with the background. In A the same
// content sat at q + f(q). Both samples are blended by t.
// Fallbacks, after SVP's: where the two samples disagree (the content was hidden in A, or the flow
// missed) B's own pixel is shown - the flow is B's, so B's side is the one that exists. Stillness is
// judged over a neighbourhood, not one pixel (two frames that merely match at one point on a flat wall
// pasted unwarped patches into moving areas): zero motion wins where it explains A better than the
// flow does - subtitles, UI and letterbox bars stay exact.
Texture2D<float4>   a   : register(t0);
Texture2D<float4>   b   : register(t1);
Texture2D<float2>   mv  : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState        samp : register(s0);
cbuffer C : register(b0) { uint w, h, ww, wh; float t; };

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float2 px = 1.0 / float2(w, h);
    const float2 uv = (float2(id.xy) + 0.5) * px;
    const float2 fs = 1.0 / float2(ww, wh);   // flow (work px) -> uv
    // stillness: A vs B in place, and A along the flow vs B, over a +-2 px cross around this pixel. The
    // in-place half first: a still pixel (most of a video frame) is decided before any flow is read.
    const float2 cross[5] = { float2(0, 0), float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2) };
    float e0 = 0, ef = 0, lb[5];
    [unroll] for (int k = 0; k < 5; ++k)
    {
        const float2 p = uv + cross[k] * px;
        lb[k] = Luma(b.SampleLevel(samp, p, 0).rgb);
        e0 += abs(Luma(a.SampleLevel(samp, p, 0).rgb) - lb[k]);
    }
    if (e0 < 0.02) { dst[id.xy] = float4(b[id.xy].rgb, 1); return; }
    // B's pixel whose content is at uv at time t
    float2 q = uv;
    [unroll] for (int i = 0; i < 3; ++i) q = uv - (1 - t) * mv.SampleLevel(samp, q, 0) * fs;
    const float2 f = mv.SampleLevel(samp, q, 0) * fs;   // B -> A, uv units
    [unroll] for (int j = 0; j < 5; ++j) ef += abs(Luma(a.SampleLevel(samp, saturate(uv + cross[j] * px + f), 0).rgb) - lb[j]);
    if (e0 < 0.5 * ef) { dst[id.xy] = float4(b[id.xy].rgb, 1); return; }
    const float3 ca = a.SampleLevel(samp, saturate(q + f), 0).rgb;
    const float3 cb = b.SampleLevel(samp, saturate(q), 0).rgb;
    const float3 d3 = abs(ca - cb);
    const float m = smoothstep(0.04, 0.12, max(d3.r, max(d3.g, d3.b)));
    dst[id.xy] = float4(lerp(lerp(ca, cb, t), cb, m), 1);
}
