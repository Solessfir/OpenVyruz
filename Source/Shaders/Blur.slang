// Dual Kawase blur (Bjorge, SIGGRAPH 2015): each pass halves or doubles the picture and samples between texels, so the linear filter averages four at a time.
struct PushConstants
{
    float2 HalfTexel; // half a texel of the source, times the spread
    float Up;         // 0 for a downsampling pass, 1 for an upsampling one
    float Extract;    // 1 keeps only the glow that the source's alpha marks
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

// A tap of the source; for the glow, only the marked part, and only where it is bright, so the floor under a faint light does not bloom.
float3 Tap(float2 Uv)
{
    const float4 Color = Source.Sample(Uv);
    if (Push.Extract < 0.5)
    {
        return Color.rgb;
    }

    const float Peak = max(Color.r, max(Color.g, Color.b));
    return Color.rgb * Color.a * smoothstep(0.5, 0.95, Peak);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput Input) : SV_Target
{
    const float2 Uv = Input.Uv;
    const float2 O = Push.HalfTexel;
    float3 Color;
    if (Push.Up < 0.5)
    {
        Color = Tap(Uv) * 4.0;
        Color += Tap(Uv - O);
        Color += Tap(Uv + O);
        Color += Tap(Uv + float2(O.x, -O.y));
        Color += Tap(Uv - float2(O.x, -O.y));
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
