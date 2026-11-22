// Clean-room MIT textured quad pixel shader for GDKX SpriteBatch::Draw().

#define SPRITE_RS \
    "RootConstants(num32BitConstants=4, b0, visibility=SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, " \
    "addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, " \
    "visibility=SHADER_VISIBILITY_PIXEL)"

Texture2D<float4> g_tex : register(t0);
SamplerState g_sam : register(s0);

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

[RootSignature(SPRITE_RS)]
float4 main(PSIn input) : SV_TARGET
{
    return g_tex.Sample(g_sam, input.uv);
}
