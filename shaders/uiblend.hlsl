// Our own UI (HUD, toasts) over the frame being presented: the premultiplied layer the text shader drew
// (text.hlsl, layer mode) blended onto the backbuffer as a render target (ONE, INV_SRC_ALPHA), one
// scissored full-screen triangle per text box. Drawn at present time on every presented frame - real,
// generated or re-projected - so frame generation never interpolates our text and the HUD keeps
// updating while the frames do not. A draw, not a compute pass: swapchains cannot be UAVs on every
// driver (Arc), and copying would lose the boxes' translucency.
Texture2D<float4> layer : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target { return layer[uint2(pos.xy)]; }
