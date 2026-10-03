// Block-matching optical flow, one pyramid level. For every B x B block of `cur`, the offset into
// `ref` (ref position = cur position + flow) with the lowest mean absolute luma difference.
//   1. predictors: the parent level's vector (x2) at this block and its 4 neighbours, then zero -
//      the neighbours are what lets a block on a motion edge pick the side it belongs to. Ties go
//      to the earliest, so a textureless block (sky, a black sphere) inherits the motion around it
//      instead of snapping to zero, which would tear it away from its own edges
//   2. a full +-R integer search around the best predictor; ties go to the centre, so flat regions
//      keep the predicted motion. lambda (per pixel of offset from the predictor) is an extra bias
//      toward it - 0 by default, every value tried on the scene sequences scored worse
//   3. (subpixel != 0) a parabolic fit through the costs either side of the minimum, per axis
// One B x B group per block; flow is written in this level's pixels.
#define B 8
#ifndef R
#define R 4   // xe_flow_r2.hlsl includes this file with R 2 for the finest levels
#endif
#define S (2 * R + 1)
#define W (B + 2 * R)
#define NP 6

Texture2D<float>    cur    : register(t0);
Texture2D<float>    ref    : register(t1);
Texture2D<float2>   parent : register(t2);   // parent level's flow grid, its pixels (unused without use_parent)
RWTexture2D<float2> flow   : register(u0);
cbuffer C : register(b0) { uint lw, lh, pw, ph, use_parent, subpixel; float lambda; };

groupshared float  s_cur[B * B];
groupshared float  s_ref[W * W];
groupshared float  s_cost[S * S];
groupshared int2   s_pred[NP];
groupshared float  s_pcost[NP];
groupshared float  s_pdiff[NP * B * B];
groupshared float  s_ppart[NP * B];
groupshared int2   s_center;

float RefAt(int2 p) { return ref.Load(int3(clamp(p, int2(0, 0), int2(lw - 1, lh - 1)), 0)); }

[numthreads(B, B, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint ti : SV_GroupIndex)
{
    const int2 org = int2(gid.xy) * B;
    s_cur[ti] = cur.Load(int3(min(org + int2(tid.xy), int2(lw - 1, lh - 1)), 0));
    if (ti < NP)
    {
        float2 p = 0;
        if (use_parent && ti < NP - 1)
        {
            const int2 off[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
            const int2 pb = clamp(int2(gid.xy >> 1) + off[ti], int2(0, 0), int2(pw - 1, ph - 1));
            p = parent.Load(int3(pb, 0)) * 2;
        }
        s_pred[ti] = int2(round(p));
    }
    GroupMemoryBarrierWithGroupSync();

    // predictor costs: every thread its own pixel for all NP predictors, then rows, then totals
    [unroll] for (uint pi = 0; pi < NP; ++pi) s_pdiff[pi * B * B + ti] = abs(s_cur[ti] - RefAt(org + int2(tid.xy) + s_pred[pi]));
    GroupMemoryBarrierWithGroupSync();
    if (ti < NP * B)
    {
        float r = 0;
        [unroll] for (uint x = 0; x < B; ++x) r += s_pdiff[ti * B + x];
        s_ppart[ti] = r;
    }
    GroupMemoryBarrierWithGroupSync();
    if (ti < NP)
    {
        float c = 0;
        [unroll] for (uint y = 0; y < B; ++y) c += s_ppart[ti * B + y];
        s_pcost[ti] = c;
    }
    GroupMemoryBarrierWithGroupSync();
    if (ti == 0)
    {
        uint best = 0;
        [unroll] for (uint i = 1; i < NP; ++i) if (s_pcost[i] < s_pcost[best]) best = i;
        s_center = s_pred[best];
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 base = org + s_center - R;
    for (uint i = ti; i < W * W; i += B * B) s_ref[i] = RefAt(base + int2(i % W, i / W));
    GroupMemoryBarrierWithGroupSync();

    for (uint c = ti; c < S * S; c += B * B)
    {
        const uint dx = c % S, dy = c / S;
        float sad = 0;
        [unroll] for (uint y = 0; y < B; ++y)
            [unroll] for (uint x = 0; x < B; ++x)
                sad += abs(s_cur[y * B + x] - s_ref[(y + dy) * W + x + dx]);
        s_cost[c] = sad / (B * B) + lambda * (abs((int)dx - R) + abs((int)dy - R));
    }
    GroupMemoryBarrierWithGroupSync();

    if (ti == 0)
    {
        uint best = S * S / 2;   // the centre wins ties
        for (uint c = 0; c < S * S; ++c) if (s_cost[c] < s_cost[best]) best = c;
        const int bx = best % S, by = best / S;
        float2 sub = 0;
        if (subpixel)
        {
            if (bx > 0 && bx < S - 1)
            {
                const float l = s_cost[best - 1], m = s_cost[best], r = s_cost[best + 1], d = l - 2 * m + r;
                if (d > 1e-6) sub.x = clamp(0.5 * (l - r) / d, -0.5, 0.5);
            }
            if (by > 0 && by < S - 1)
            {
                const float u = s_cost[best - S], m = s_cost[best], dn = s_cost[best + S], d = u - 2 * m + dn;
                if (d > 1e-6) sub.y = clamp(0.5 * (u - dn) / d, -0.5, 0.5);
            }
        }
        flow[gid.xy] = float2(s_center + int2(bx, by) - R) + sub;
    }
}
