// Dual Kawase blur (Bjorge, SIGGRAPH 2015): each pass halves or doubles the picture and samples between texels, so the linear filter averages four at a time.
struct PushConstants
{
    float2 HalfTexel; // half a texel of the source, times the spread
    float Up;         // 0 for a downsampling pass, 1 for an upsampling one
    float Padding;
    float4 Tint;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> Push;
[[vk::binding(0, 0)]] Sampler2D Source;

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

// One triangle over the whole target.
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
    const float2 Uv = Input.Uv;
    const float2 O = Push.HalfTexel;
    float3 Color;
    if (Push.Up < 0.5)
    {
        Color = Source.Sample(Uv).rgb * 4.0;
        Color += Source.Sample(Uv - O).rgb;
        Color += Source.Sample(Uv + O).rgb;
        Color += Source.Sample(Uv + float2(O.x, -O.y)).rgb;
        Color += Source.Sample(Uv - float2(O.x, -O.y)).rgb;
        Color /= 8.0;
    }
    else
    {
        Color = Source.Sample(Uv + float2(-O.x * 2.0, 0.0)).rgb;
        Color += Source.Sample(Uv + float2(-O.x, O.y)).rgb * 2.0;
        Color += Source.Sample(Uv + float2(0.0, O.y * 2.0)).rgb;
        Color += Source.Sample(Uv + float2(O.x, O.y)).rgb * 2.0;
        Color += Source.Sample(Uv + float2(O.x * 2.0, 0.0)).rgb;
        Color += Source.Sample(Uv + float2(O.x, -O.y)).rgb * 2.0;
        Color += Source.Sample(Uv + float2(0.0, -O.y * 2.0)).rgb;
        Color += Source.Sample(Uv + float2(-O.x, -O.y)).rgb * 2.0;
        Color /= 12.0;
    }

    return float4(Color * Push.Tint.rgb, 1.0);
}
