// Frame interpolation at time t in (0, 1) between prev (t = 0) and cur (t = 1), from the two flow
// grids of xe_flow: fwd (prev -> cur, on prev's pixels) and bwd (cur -> prev, on cur's pixels).
//
// 1. Flow from the intermediate frame to each real one, from the flows at the same pixel (Jiang et
//    al., Super SloMo, eq. 4 - exact for linear motion where both flows see the same surface):
//        Ft0 = -(1-t) t F01 + t^2 F10        Ft1 = (1-t)^2 F01 - t (1-t) F10
// 2. Sample prev at x + Ft0 and cur at x + Ft1.
// 3. Visibility per side from forward-backward consistency AT THE SAMPLED POSITION: a pixel of prev
//    whose forward vector does not come back through cur's backward vector is covered in cur (or the
//    flow is wrong there), so it should not be shown. Tolerance grows with the motion (block flow is
//    coarse on fast content). A side that fails loses its weight; the time weights ((1-t), t) blend
//    the rest.
// Flow grids are in L0 pixels, one vector per 8x8 L0 block: `f` native pixels per L0 pixel.
Texture2D<float4>   prev : register(t0);
Texture2D<float4>   cur  : register(t1);
Texture2D<float2>   fwd  : register(t2);
Texture2D<float2>   bwd  : register(t3);
RWTexture2D<float4> dst  : register(u0);
SamplerState        lin  : register(s0);
cbuffer C : register(b0) { uint w, h, gw, gh; float f, t, tol, tol_rel; uint debug; };

float2 Flow(Texture2D<float2> g, float2 p) { return g.SampleLevel(lin, p / (f * 8.0 * float2(gw, gh)), 0) * f; }
float4 Color(Texture2D<float4> c, float2 p) { return c.SampleLevel(lin, p / float2(w, h), 0); }

float Visible(Texture2D<float2> there, Texture2D<float2> back, float2 p)
{
    const float2 v = Flow(there, p);
    const float e = length(v + Flow(back, p + v)), k = tol + tol_rel * length(v);
    return exp(-(e * e) / (k * k));
}

// The frame at time t at pixel centre x (also the body of xe_interp_gfx's pixel shader).
float4 Interpolate(float2 x)
{
    const float2 f01 = Flow(fwd, x), f10 = Flow(bwd, x);
    const float2 p0 = x + (-(1 - t) * t * f01 + t * t * f10);
    const float2 p1 = x + ((1 - t) * (1 - t) * f01 - t * (1 - t) * f10);
    const float v0 = Visible(fwd, bwd, p0), v1 = Visible(bwd, fwd, p1);
    // The floor keeps a both-sides-rejected pixel on the time blend; scaling it by the time weights keeps
    // t = 0 and t = 1 exactly the real frames (the presenter shows real frames as t = 1).
    const float w0 = (1 - t) * (v0 + 1e-3), w1 = t * (v1 + 1e-3);
    float4 c = (w0 * Color(prev, p0) + w1 * Color(cur, p1)) / (w0 + w1);
    if (debug) c = float4(1 - v0, 1 - v1, 0, 1);   // red: prev side rejected, green: cur side
    return float4(c.rgb, 1);
}

#ifndef XE_INTERP_NO_CS
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    dst[id.xy] = Interpolate(float2(id.xy) + 0.5);
}
#endif
