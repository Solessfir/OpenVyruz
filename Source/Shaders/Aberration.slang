// The port's hit flash: red and blue drawn apart from green, outward from the middle and more toward the edges, as a lens fringes.
struct PushConstants
{
    float2 MaxOffset; // the split at the view's edge, in UV
    float2 Padding;
    float4 Unused;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> Push;
[[vk::binding(0, 0)]] Sampler2D Source;

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

// One triangle over the whole view.
[shader("vertex")]
VertexOutput vertexMain(uint VertexIndex : SV_VertexID)
{
    const float2 Uv = float2((VertexIndex << 1) & 2, VertexIndex & 2);
    VertexOutput Output;
    Output.Position = float4(Uv * 2.0 - 1.0, 0.0, 1.0);
    Output.Uv = Uv;
    return Output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput Input) : SV_Target
{
    // -1 to 1 across the view; the split grows with the square of the distance from the middle.
    const float2 FromMiddle = Input.Uv * 2.0 - 1.0;
    const float2 Offset = FromMiddle * dot(FromMiddle, FromMiddle) * 0.5 * Push.MaxOffset;
    const float Red = Source.Sample(Input.Uv - Offset).r;
    const float Green = Source.Sample(Input.Uv).g;
    const float Blue = Source.Sample(Input.Uv + Offset).b;
    return float4(Red, Green, Blue, 1.0);
}
