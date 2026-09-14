// OFA flow grid (R16G16_SINT, S10.5, units = OFA-input pixels) -> motion vectors (R16G16_FLOAT,
// mw x mh, units = work-resolution pixels). Bilinear over the grid cells; zeroed on reset, below
// zero_below px, or (use_cost) when any of the 4 cells has cost > cost_reject.
Texture2D<int2>     flow : register(t0);
Texture2D<uint>     cost : register(t1);   // may be a stand-in when use_cost == 0
RWTexture2D<float2> mv   : register(u0);
cbuffer C : register(b0)
{
    uint  fw, fh, ofa_w, ofa_h;
    uint  mw, mh, grid;
    float zero_below;
    uint  cost_reject, use_cost, reset;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= mw || id.y >= mh) return;
    const int2 limit = int2(fw - 1, fh - 1);
    const float2 q = (float2(id.xy) + 0.5) * float2(ofa_w, ofa_h) / float2(mw, mh) / grid - 0.5;
    const int2 a = int2(floor(q));
    const float2 t = frac(q);
    const int2 c00 = clamp(a, 0, limit), c10 = clamp(a + int2(1, 0), 0, limit);
    const int2 c01 = clamp(a + int2(0, 1), 0, limit), c11 = clamp(a + int2(1, 1), 0, limit);
    float2 v = lerp(lerp(float2(flow[c00]), float2(flow[c10]), t.x),
                    lerp(float2(flow[c01]), float2(flow[c11]), t.x), t.y);
    v = v / 32.0 * float2(mw, mh) / float2(ofa_w, ofa_h);
    bool zero = reset || length(v) < zero_below;
    if (use_cost)
        zero = zero || max(max(cost[c00], cost[c10]), max(cost[c01], cost[c11])) > cost_reject;
    mv[id.xy] = zero ? float2(0, 0) : v;
}
