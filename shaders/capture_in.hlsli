// A captured pixel as the 8-bit pipeline sees it. white = 0: the capture is BGRA8 already (its SRV returns
// RGB order). white > 0: an HDR desktop duplicated as FP16 scRGB (linear, 1.0 = 80 nits) - SDR content
// there is sRGB decoded and scaled to the SDR white level (white = level / 80 nits), so dividing by it and
// re-encoding gives back exactly what the app drew. Brighter-than-SDR-white highlights clip. Windows'
// own conversion to BGRA8 maps 80 nits to white instead, which came out far too bright.
float4 CaptureIn(Texture2D<float4> t, uint2 q, float white)
{
    float4 c = t[q];
    if (white > 0)
    {
        const float3 l = saturate(c.rgb / white);
        c = float4(l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055, 1);
    }
    return c;
}
