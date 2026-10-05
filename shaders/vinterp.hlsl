// engine=video: the frame at time t (0..1) between two real frames A and B, for 2D video timed by its own
// clock (fg.cpp VideoPresent). mv is the pipeline's backward flow of B (where B's content was in A, work
// pixels). Content at output pixel p sat at p + t*f in A and is at p - (1-t)*f in B - the flow sampled at
// p stands in for the flow along the path (smooth motion). Both warps are blended by t.
// Fallbacks, after SVP's: where the two warped samples disagree (occlusion, flow that missed) the nearer
// frame's sample is shown instead of a double image; a pixel that did not change at all (subtitles, UI,
// letterbox bars) is copied exactly.
Texture2D<float4>   a   : register(t0);
Texture2D<float4>   b   : register(t1);
Texture2D<float2>   mv  : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState        samp : register(s0);
cbuffer C : register(b0) { uint w, h, ww, wh; float t; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float4 a0 = a[id.xy], b0 = b[id.xy];
    if (all(abs(a0.rgb - b0.rgb) < 1.5 / 255)) { dst[id.xy] = float4(b0.rgb, 1); return; }
    const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    const float2 f = mv.SampleLevel(samp, uv, 0) / float2(ww, wh);   // uv units, B -> A
    const float3 ca = a.SampleLevel(samp, saturate(uv + t * f), 0).rgb;
    const float3 cb = b.SampleLevel(samp, saturate(uv - (1 - t) * f), 0).rgb;
    const float3 d3 = abs(ca - cb);
    const float k = smoothstep(0.06, 0.2, max(d3.r, max(d3.g, d3.b)));
    dst[id.xy] = float4(lerp(lerp(ca, cb, t), t < 0.5 ? ca : cb, k), 1);
}
