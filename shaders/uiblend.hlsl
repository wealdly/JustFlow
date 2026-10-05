// Our UI (HUD, toasts): the premultiplied layer text.hlsl drew (layer mode), blended onto the backbuffer
// (ONE, INV_SRC_ALPHA) as one scissored full-screen triangle per text box, on every presented frame - so FG
// never interpolates our text. A draw, not compute: swapchains cannot be UAVs on every driver (Arc).
Texture2D<float4> layer : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target { return layer[uint2(pos.xy)]; }
