// Sprite quad in the original 640x480 screen space; the viewport letterboxes it into the window.
struct PushConstants
{
    float4 Rect;   // x, y, width, height in screen pixels
    float4 UvRect; // u0, v0, u1, v1
    float2 ScreenSize;
    float2 OutlineTexel; // a texel's size in UV; non-zero draws the sprite's outline instead

    float4 Diffuse;
    float4 Specular;
    float4 Rotation; // cos, sin, pivot x, pivot y: DrawAnimationFrame turns the frame about the object's position
    float4 Soft;     // x non-zero: the port's own premultiplied image, filtered smoothly and blended by its alpha
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
    const float2 Unturned = Push.Rect.xy + Corner * Push.Rect.zw - Push.Rotation.zw;
    const float2 Pixel = Push.Rotation.zw + float2(Unturned.x * Push.Rotation.x - Unturned.y * Push.Rotation.y, Unturned.x * Push.Rotation.y + Unturned.y * Push.Rotation.x);

    VertexOutput Output;
    Output.Position = float4(Pixel / Push.ScreenSize * 2.0 - 1.0, 0.0, 1.0);
    Output.Uv = lerp(Push.UvRect.xy, Push.UvRect.zw, Corner);
    return Output;
}

// Where to sample so each texel stays a sharp block at any scale, with only the screen pixel across a texel seam blended.
// Nearest filtering at a non-integer scale gives texels uneven widths that crawl as the view scrolls; this keeps them even and smooths turned sprites.
float2 PixelArtUv(float2 Uv)
{
    float Width;
    float Height;
    SpriteTexture.GetDimensions(Width, Height);
    const float2 Size = float2(Width, Height);
    const float2 Texel = Uv * Size;
    const float2 Seam = floor(Texel + 0.5);
    const float2 Footprint = max(fwidth(Texel), 1e-5);
    return (Seam + clamp((Texel - Seam) / Footprint, -0.5, 0.5)) / Size;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput Input) : SV_Target
{
    if (Push.Soft.x > 0.0)
    {
        const float4 Image = SpriteTexture.Sample(Input.Uv);
        if (Image.a <= 0.0)
        {
            discard;
        }

        return float4(Image.rgb / Image.a * Push.Diffuse.rgb, Image.a * Push.Diffuse.a);
    }

    const float2 Uv = PixelArtUv(Input.Uv);
    if (Push.OutlineTexel.x > 0.0)
    {
        // The outline covers what the sprite itself covers, which the stencil passes saw, and finds its edge by whole texels.
        if (SpriteTexture.Sample(Uv).a < 0.5)
        {
            discard;
        }

        const float2 Centre = (floor(Input.Uv / Push.OutlineTexel) + 0.5) * Push.OutlineTexel;

        // An opaque texel with a see-through neighbour, or one past the frame's own rectangle, is the edge: drawn soft, the rest a faint fill, neither fully opaque.
        const float EdgeAlpha = 0.55;
        const float FillAlpha = 0.12;
        const float2 Low = min(Push.UvRect.xy, Push.UvRect.zw);
        const float2 High = max(Push.UvRect.xy, Push.UvRect.zw);
        const float2 Steps[4] = { float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0) };
        float Edge = 0.0;
        for (int Index = 0; Index < 4; ++Index)
        {
            const float2 Neighbour = Centre + Steps[Index] * Push.OutlineTexel;
            const bool bInside = all(Neighbour >= Low) && all(Neighbour <= High);
            if (!bInside || SpriteTexture.Sample(Neighbour).a < 0.5)
            {
                Edge = 1.0;
            }
        }

        return float4(Push.Diffuse.rgb, lerp(FillAlpha, EdgeAlpha, Edge) * Push.Diffuse.a);
    }

    // Color-keyed pixels are uploaded as zero, so dividing by the alpha takes their share out of a blend with an opaque neighbour, and the cut stays on the texel seam.
    const float4 Color = SpriteTexture.Sample(Uv);
    if (Color.a < 0.5)
    {
        discard;
    }
    return float4(Color.rgb / Color.a * Push.Diffuse.rgb + Push.Specular.rgb, Push.Diffuse.a);
}
