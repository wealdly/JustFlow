// On-screen text: one ASCII string (32..126, up to 64 chars packed 4 per uint) drawn at 8*scale px
// per glyph, white with a 1 px dark outline, on a rounded dark box (alpha 0.6 * alpha). The dispatch
// covers the box only: thread (x, y) = box pixel (x, y), box top-left at (x0, y0) in dst.
Texture2D<uint>     font : register(t0);   // 95 x 8 R8_UINT: texel (glyph, row) = bitmask, bit i = column i
RWTexture2D<float4> dst  : register(u0);
cbuffer C : register(b0)
{
    int   x0, y0;
    uint  scale;
    float alpha;
    int   pad;             // box padding = corner radius
    uint  len, w, h;
    uint4 text[4];         // 64 chars
};

// 1 when the text pixel (lx, ly) (box-local, pad removed) is lit.
uint Glyph(int lx, int ly)
{
    if (lx < 0 || ly < 0) return 0;
    const uint ci = (uint)lx / (8 * scale), row = (uint)ly / scale;
    if (ci >= len || row >= 8) return 0;
    const uint ch = (text[ci >> 4][(ci >> 2) & 3] >> ((ci & 3) * 8)) & 0xFF;
    if (ch < 32 || ch > 126) return 0;
    const uint col = ((uint)lx / scale) & 7;
    return (font[uint2(ch - 32, row)] >> col) & 1;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int bw = (int)(len * 8 * scale) + 2 * pad, bh = (int)(8 * scale) + 2 * pad;
    const int2 q = int2(id.xy);
    if (q.x >= bw || q.y >= bh) return;
    const int2 p = int2(x0, y0) + q;
    if (p.x < 0 || p.y < 0 || p.x >= (int)w || p.y >= (int)h) return;
    // rounded box: inside when within `pad` of the deflated rectangle
    const int2 d = q - clamp(q, int2(pad, pad), int2(bw - 1 - pad, bh - 1 - pad));
    if (d.x * d.x + d.y * d.y > pad * pad) return;
    float3 c = lerp(dst[p].rgb, float3(0.04, 0.04, 0.05), 0.6 * alpha);
    const int lx = q.x - pad, ly = q.y - pad;
    if (Glyph(lx, ly)) c = lerp(c, 1.0, alpha);
    else
    {
        uint o = 0;
        [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) o |= Glyph(lx + dx, ly + dy);
        if (o) c = lerp(c, 0.0, alpha);
    }
    dst[p] = float4(c, 1);
}
