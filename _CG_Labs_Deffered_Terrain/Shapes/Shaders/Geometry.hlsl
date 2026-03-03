// Geometry.hlsl — геометрический проход отложенного рендеринга (G-Buffer).
// VS: использует ObjectCB (b0) и PassCB (b2), выдаёт позицию в clip space.
// PS: записывает в два целевых буфера — Albedo (SV_Target0) и Normal (SV_Target1).
// Текстуры: t0 = albedo, t1 = normal.

#define MaxLights 32

struct Light
{
    float3 Strength;
    float FalloffStart;
    
    float3 Direction;
    float FalloffEnd;
    
    float3 Position;
    float SpotPower;

    int Type;
    float3 pad;
};

Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
SamplerState gSampler : register(s0);

cbuffer ObjectConstants : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4x4 gTexTransform;
};

cbuffer MaterialConstants : register(b1)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float4x4 gMatTransform;
};

cbuffer PassConstants : register(b2)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;

    float3 gEyePosW;
    float cbPerObjectPad1;

    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;

    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;

    float4 gAmbientLight;

    Light gLights[MaxLights]; // MaxLights = 8
};

struct VSInput
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD0;
    float3 TangentU : TANGENT;
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : TEXCOORD1;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD0;
    float3 TangentW : TEXCOORD2;
};

VSOutput VS(VSInput vin)
{
    VSOutput vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.PosH = mul(float4(vout.PosW, 1.0f), gViewProj);
    vout.NormalW = normalize(mul(float4(vin.NormalL, 0.0f), gWorldInvTranspose).xyz);
    vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);
    vout.TexC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform).xy;
    return vout;
}


struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
};

GBufferOut PS(VSOutput pin)
{
    GBufferOut pout;
    
    float4 tex = gDiffuseMap.Sample(gSampler, pin.TexC);
    pout.Albedo = tex * gDiffuseAlbedo;

    float3 nMap = gNormalMap.Sample(gSampler, pin.TexC).xyz * 2.0f - 1.0f;
    float3 N = normalize(pin.NormalW);
    float3 T = normalize(pin.TangentW);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3( T, B, N );
    float3 n = normalize(mul(nMap, TBN));
    pout.Normal = float4(n, 1.0f);
        
    return pout;
}
