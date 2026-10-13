// Sprite quad in the original 640x480 screen space; the viewport letterboxes it into the window.
struct PushConstants
{
    float4 Rect;   // x, y, width, height in screen pixels
    float4 UvRect; // u0, v0, u1, v1
    float2 ScreenSize;
    float2 Padding;
    float4 Diffuse;
    float4 Specular;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> Push;
[[vk::binding(0, 0)]] Sampler2D SpriteTexture;

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint VertexIndex : SV_VertexID)
{
    const float2 Corner = float2(VertexIndex & 1, VertexIndex >> 1);
    const float2 Pixel = Push.Rect.xy + Corner * Push.Rect.zw;

    VertexOutput Output;
    Output.Position = float4(Pixel / Push.ScreenSize * 2.0 - 1.0, 0.0, 1.0);
    Output.Uv = lerp(Push.UvRect.xy, Push.UvRect.zw, Corner);
    return Output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput Input) : SV_Target
{
    const float4 Color = SpriteTexture.Sample(Input.Uv);
    // Color-keyed pixels are uploaded with zero alpha.
    if (Color.a < 0.5)
    {
        discard;
    }
    return float4(Color.rgb * Push.Diffuse.rgb + Push.Specular.rgb, Color.a);
}
