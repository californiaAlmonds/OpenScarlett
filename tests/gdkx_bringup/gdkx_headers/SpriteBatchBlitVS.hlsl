// Clean-room MIT textured quad vertex shader for GDKX SpriteBatch::Draw().

#define SPRITE_RS \
    "RootConstants(num32BitConstants=4, b0, visibility=SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, " \
    "addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, " \
    "visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer SpriteRectCB : register(b0)
{
    float4 g_ndcRect; // (left, top, right, bottom) in NDC
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

[RootSignature(SPRITE_RS)]
VSOut main(uint vid : SV_VertexID)
{
    static const float2 kUv[6] = {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 1.0f),
    };

    float2 uv = kUv[vid % 6u];
    VSOut o;
    o.pos = float4(lerp(g_ndcRect.x, g_ndcRect.z, uv.x),
                   lerp(g_ndcRect.y, g_ndcRect.w, uv.y),
                   0.0f,
                   1.0f);
    o.uv = uv;
    return o;
}
