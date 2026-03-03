// GeometryTAA.hlsl — геометрический проход отложенного рендеринга (G-Buffer) с поддержкой TAA.
// VS: использует ObjectCB (b0) и PassCB (b2), выдаёт позицию в clip space с применением джиттера.
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
    
    // world матрица предыдущего кадра для velocity computation
    float4x4 gPrevWorld;
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
    
    // матрицы предыдущего кадра для velocity computation
    float4x4 gPrevView;
    float4x4 gPrevProj;
    float4x4 gPrevViewProj;
    
    // Jittered матрицы для TAA
    float4x4 gProjJittered;
    float4x4 gViewProjJittered;
    float4x4 gInvViewProjJittered;
    float2 gPrevViewportJitter;
    float2 cbPad0;

    float3 gEyePosW;
    float cbPerObjectPad1;

    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;

    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;

    float4 gAmbientLight;

    // джиттер, [-0.5, 0.5]
    float2 gViewportJitter;
    float2 gTAAModulation; // x = modulation factor, y = ниче, pad

    Light gLights[MaxLights]; // MaxLights 8
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
    float4 PosH : SV_POSITION;           // Jittered position for rasterization
    float3 PosW : TEXCOORD1;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD0;
    float3 TangentW : TEXCOORD2;
    
    // Positions for velocity computation
    float4 CurrentPosClip : TEXCOORD3;   // Current position without jitter
    float4 PrevPosClip : TEXCOORD4;      // Previous frame position
};

VSOutput VS(VSInput vin)
{
    VSOutput vout;
    
    // Transform to world space
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    
    // Use jittered ViewProj for rasterization
    vout.PosH = mul(float4(vout.PosW, 1.0f), gViewProjJittered);
    
    // Use non-jittered ViewProj for velocity computation
    vout.CurrentPosClip = mul(float4(vout.PosW, 1.0f), gViewProj);
    
    // Previous frame position (reprojection)
    float4 prevPosW = mul(float4(vin.PosL, 1.0f), gPrevWorld);
    vout.PrevPosClip = mul(float4(prevPosW.xyz, 1.0f), gPrevViewProj);
    
    // Existing normal/tangent/texcoord transforms
    vout.NormalW = normalize(mul(float4(vin.NormalL, 0.0f), gWorldInvTranspose).xyz);
    vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);
    vout.TexC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform).xy;
    
    return vout;
}


struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float2 Velocity : SV_Target2;  // Velocity buffer output
};


// Compute velocity with numerical stability checks
float2 CalcVelocity(float4 currentClip, float4 prevClip)
{
    const float EPSILON = 0.0001f;
    const float MAX_VELOCITY = 0.5f; // Clamp to 50% of screen
    
    // Check for invalid w components
    if (abs(currentClip.w) < EPSILON || abs(prevClip.w) < EPSILON)
    {
        // Invalid w component - return zero velocity
        return float2(0.0f, 0.0f);
    }
    
    // Compute velocity in NDC space
    // CurrentPosClip and PrevPosClip are computed WITHOUT jitter
    float2 currentNDC = currentClip.xy / currentClip.w;
    float2 prevNDC = prevClip.xy / prevClip.w;
    
    // Velocity in NDC space (current - previous)
    float2 velocityNDC = currentNDC - prevNDC;
    
    // Convert NDC velocity to UV velocity
    // NDC: X right = +1, Y up = +1
    // UV:  X right = +1, Y down = +1
    // Scale: NDC [-1,1] -> UV [0,1] means multiply by 0.5
    // Flip Y: NDC +Y is up, UV +Y is down, so negate Y
    float2 velocity = velocityNDC * float2(0.5f, -0.5f);
    
    // Clamp extreme velocities
    float velocityMag = length(velocity);
    if (velocityMag > MAX_VELOCITY)
    {
        velocity = normalize(velocity) * MAX_VELOCITY;
    }
    
    // Check for NaN/Inf
    if (any(isnan(velocity)) || any(isinf(velocity)))
    {
        return float2(0.0f, 0.0f);
    }
    
    return velocity;
}

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
    
    // Encode normal from [-1, 1] to [0, 1] range for storage
    pout.Normal = float4(n * 0.5f + 0.5f, 1.0f);
    
    // Compute and write velocity
    float2 velocity = CalcVelocity(pin.CurrentPosClip, pin.PrevPosClip);
    
    // DEBUG: Encode velocity magnitude in alpha channel for debugging
    float velocityMag = length(velocity);
    
    // Write velocity to buffer
    // Store magnitude in unused channels for debugging (can visualize later)
    pout.Velocity = velocity;
    
    return pout;
}
