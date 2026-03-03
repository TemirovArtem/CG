// Lighting.hlsl
// Lighting pass: fullscreen triangle VS and PS that samples G-Buffer at t10 / t11
// t10 = Albedo SRV, t11 = Normal SRV (these are placed starting at register t10 in the SRV heap).
// Pass constants includes ambient and lights (we read Lights array here).


// ---------- Defines ----------
#define MaxLights 32
#define LIGHT_TYPE_DIRECTIONAL 3
#define LIGHT_TYPE_SPOT        1
#define LIGHT_TYPE_POINT       MaxLights - LIGHT_TYPE_DIRECTIONAL - LIGHT_TYPE_SPOT

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


// ---------- CBs ----------
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
    
    // NEW: Previous frame matrices for velocity computation (must match CPU structure)
    float4x4 gPrevView;
    float4x4 gPrevProj;
    float4x4 gPrevViewProj;
    
    // NEW: Jittered matrices for TAA
    float4x4 gProjJittered;
    float4x4 gViewProjJittered;
    float4x4 gInvViewProjJittered;
    float2 gPrevViewportJitter;
    float2 cbPad0;  // Padding for 16-byte alignment

    float3 gEyePosW;
    float cbPerObjectPad1;

    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;

    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;

    float4 gAmbientLight;

    // TAA: джиттер и модуляция
    float2 gViewportJitter;
    float2 gTAAModulation;

    Light gLights[MaxLights];
};

// ---------- Textures / sampler ----------
Texture2D gAlbedoTex : register(t2);
Texture2D gNormalTex : register(t3); 
Texture2D gDepthTex : register(t4);
SamplerState gSampler : register(s0);

// ---------- VS for full-screen triangle ----------
struct VSOutput
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

float CalcAttenuation(float d, float falloffStart, float falloffEnd)
{
    // Linear falloff.
    return saturate((falloffEnd - d) / (falloffEnd - falloffStart));
}

float3 ReconstructPosition(float2 uv, float depth)
{
    // uv -> clip space
    float4 clipPos = float4(uv * 2.0f - 1.0f, depth, 1.0f);

    // back to world space
    float4 worldPos = mul(clipPos, gInvViewProj);
    worldPos /= worldPos.w;

    return worldPos.xyz;
}

float3 ComputeWorldPos(float2 texcoord, float depth)
{   
    // Convert depth from [0,1] to [-1,1] clip space range
    float clipZ = depth * 2.0f - 1.0f;
    
    // Reconstruct using the proper perspective correct formula
    // depth comes in as [0,1] from depth buffer
    float4 clipPos = float4(texcoord.x * 2.0f - 1.0f, 1.0f - texcoord.y * 2.0f, clipZ, 1.0f);
    // FIXED: Use jittered inverse projection to match the jittered projection used during depth rendering
    float4 viewPos = mul(clipPos, gInvViewProjJittered);
    // Perspective divide to get world position
    return viewPos.xyz / viewPos.w;
}

float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
    float cosIncidentAngle = saturate(dot(normal, lightVec));

    float f0 = 1.0f - cosIncidentAngle;
    float3 reflectPercent = R0 + (1.0f - R0) * (f0 * f0 * f0 * f0 * f0);

    return reflectPercent;
}

float3 BlinnPhong(float3 diffuseAlbedo, float3 lightStrength, float3 lightVec, float3 normal, float3 toEye)
{
    // Normalize inputs
    float3 norm = normalize(normal);
    float3 lightDir = normalize(lightVec);
    float3 viewDir = normalize(toEye);
    float3 halfVec = normalize(viewDir + lightDir);
    
    // Diffuse component using Lambert's cosine law
    float NdotL = saturate(dot(norm, lightDir));
    float3 diffuse = diffuseAlbedo * NdotL;
    
    // Specular component using Blinn-Phong
    float NdotH = saturate(dot(norm, halfVec));
    float specularPower = max(4.0f, (1 - gRoughness) * 64.0f); // Adjusted for better control
    float specularIntensity = pow(NdotH, specularPower);
    
    // Fresnel factor (Schlick approximation)
    float3 fresnelFactor = SchlickFresnel(gFresnelR0, norm, viewDir);
    
    // Combine specular with fresnel
    float3 specular = specularIntensity * fresnelFactor;
    
    // Energy conservation - ensure that diffuse + specular doesn't exceed 1.0
    float energyFactor = 1.0f - min(0.5f, max(specular.r, max(specular.g, specular.b)));
    diffuse *= energyFactor;
    
    // Final result
    float3 result = (diffuse + specular) * lightStrength;
    
    return result;
}

//---------------------------------------------------------------------------------------
// Evaluates the lighting equation for directional lights.
//---------------------------------------------------------------------------------------
float3 ComputeDirectionalLight(Light L, float3 diffuseAlbedo, float3 normal, float3 toEye)
{
    float3 lightVec = -L.Direction;
    
    // Ensure vectors are normalized
    lightVec = normalize(lightVec);
    normal = normalize(normal);
    toEye = normalize(toEye);
    
    // Calculate N·L with better handling for edge cases
    float ndotl = saturate(dot(lightVec, normal));
    
    // Early exit if no contribution
    if (ndotl <= 0.0f) return 0.0f;
    
    float3 lightStrength = L.Strength * ndotl;
    
    return BlinnPhong(diffuseAlbedo, lightStrength, lightVec, normal, toEye);
}

//---------------------------------------------------------------------------------------
// Evaluates the lighting equation for point lights.
//---------------------------------------------------------------------------------------
float3 ComputePointLight(Light L, float3 pos, float3 diffuseAlbedo, float3 normal, float3 toEye)
{
    float3 lightVec = L.Position - pos;
    float d = length(lightVec);
    if (d > L.FalloffEnd)
        return 0.0f;
        
    // Normalize light vector
    lightVec /= d;
    
    // Ensure vectors are normalized
    normal = normalize(normal);
    toEye = normalize(toEye);
    
    float ndotl = saturate(dot(lightVec, normal));
    
    // Early exit if no contribution
    if (ndotl <= 0.0f) return 0.0f;
    
    float3 lightStrength = L.Strength * ndotl;
    float att = CalcAttenuation(d, L.FalloffStart, L.FalloffEnd);
    lightStrength *= att;
    return BlinnPhong(diffuseAlbedo, lightStrength, lightVec, normal, toEye);
}

//---------------------------------------------------------------------------------------
// Evaluates the lighting equation for spot lights.
//---------------------------------------------------------------------------------------
float3 ComputeSpotLight(Light L, float3 pos, float3 diffuseAlbedo, float3 normal, float3 toEye)
{
    float3 lightVec = L.Position - pos;
    float d = length(lightVec);
    if (d > L.FalloffEnd)
        return 0.0f;
        
    // Normalize light vector
    lightVec = normalize(lightVec);
    
    // Ensure vectors are normalized
    normal = normalize(normal);
    toEye = normalize(toEye);
    
    float ndotl = saturate(dot(lightVec, normal));
    
    // Early exit if no contribution
    if (ndotl <= 0.0f) return 0.0f;
    
    float3 lightStrength = L.Strength * ndotl;
    float att = CalcAttenuation(d, L.FalloffStart, L.FalloffEnd);
    lightStrength *= att;
    float spotFactor = pow(saturate(dot(-lightVec, L.Direction)), L.SpotPower);
    lightStrength *= spotFactor;
    return BlinnPhong(diffuseAlbedo, lightStrength, lightVec, normal, toEye);
}

float4 ComputeLight(Light gLights[MaxLights], float3 diffuseAlbedo, float3 posW, float3 normalW, float3 toEye)
{
    float3 result = 0.0f;
    int i = 0;
    
    if (LIGHT_TYPE_DIRECTIONAL > 0)
    {
        for (i = 0; i < LIGHT_TYPE_DIRECTIONAL; ++i)
            result += ComputeDirectionalLight(gLights[i], diffuseAlbedo, normalW, toEye);
    }
    if (LIGHT_TYPE_SPOT > 0)
    {
        for (i = LIGHT_TYPE_DIRECTIONAL; i < LIGHT_TYPE_DIRECTIONAL + LIGHT_TYPE_SPOT; ++i)
            result += ComputeSpotLight(gLights[i], posW, diffuseAlbedo, normalW, toEye);
    }
    if (LIGHT_TYPE_POINT > 0)
    {
        for (i = LIGHT_TYPE_DIRECTIONAL + LIGHT_TYPE_SPOT; i < LIGHT_TYPE_DIRECTIONAL + LIGHT_TYPE_POINT + LIGHT_TYPE_SPOT; ++i)
            result += ComputePointLight(gLights[i], posW, diffuseAlbedo, normalW, toEye);
    }

    return float4(result, 1.0);
}

float4 ComputeLighting(Light gLights[MaxLights], float3 diffuseAlbedo, float3 posW, float3 normalW, float3 toEye)
{
    float3 result = 0.0f;
    
    // DEBUG: Count how many lights contribute
    int contributingLights = 0;
    
    for (int i = 0; i < MaxLights; ++i)
    {
        float3 contribution = 0.0f;
        
        if (gLights[i].Type == 0)
            contribution = ComputeDirectionalLight(gLights[i], diffuseAlbedo, normalW, toEye);
        else if (gLights[i].Type == 1)
            contribution = ComputePointLight(gLights[i], posW, diffuseAlbedo, normalW, toEye);
        else if (gLights[i].Type == 2)
            contribution = ComputeSpotLight(gLights[i], posW, diffuseAlbedo, normalW, toEye);
        
        // Check if this light contributes anything
        if (length(contribution) > 0.001f)
        {
            contributingLights++;
        }
        
        result += contribution;
    }
    
    return float4(result, 1.0);
}


VSOutput VS(uint id : SV_VertexID) // 
{ 
    float2 verts[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };
    
    VSOutput vout;
    
    vout.PosH = float4(verts[id], 0.0, 1.0);
    vout.TexC = 0.5f * (verts[id] + 1.0f); // tex = Ã¨Ã§ [-1..1] Ã¢ [0..1]
    vout.TexC.y = 1.0f - vout.TexC.y; // Flip texture Y upside-down
    
    return vout;
}

// ---------- PS: do lighting, write to backbuffer ----------
float4 PS(VSOutput pin) : SV_Target 
{
    // Sample G-buffer
    float4 albedo = gAlbedoTex.Sample(gSampler, pin.TexC);
    float3 normal = gNormalTex.Sample(gSampler, pin.TexC).xyz;
    normal = normalize(normal * 2.0f - 1.0f);
    float depth = gDepthTex.Sample(gSampler, pin.TexC).r;

    // Reconstruct world position
    float3 posW = ComputeWorldPos(pin.TexC, depth);
    
    // Calculate view direction (from surface point to camera)
    float3 viewDir = normalize(gEyePosW - posW);
    
    // Ambient term
    float4 ambient = gAmbientLight * albedo;

    // Compute lighting from all lights
    float4 directLight = ComputeLighting(gLights, albedo.rgb, posW, normal, viewDir);

    // Clamp final color to prevent overflow
    float4 color = saturate(ambient + directLight);

    // DEBUG OUTPUTS - uncomment one at a time to debug
    //return albedo;                                    // Check albedo texture
    //return float4(normal * 0.5f + 0.5f, 1.0f);       // Check normals (should be colorful)
    //return float4(depth, depth, depth, 1.0f);        // Check depth
    //return ambient;                                   // Check ambient only
    //return directLight;                               // Check direct lighting only
    
    // Check individual lights (uncomment to test each light)
    //return float4(ComputeDirectionalLight(gLights[0], albedo.rgb, normal, viewDir), 1.0f);  // Light 0 (directional)
    //return float4(ComputeDirectionalLight(gLights[1], albedo.rgb, normal, viewDir), 1.0f);  // Light 1 (directional)
    //return float4(ComputeDirectionalLight(gLights[2], albedo.rgb, normal, viewDir), 1.0f);  // Light 2 (directional)
    //return float4(ComputeSpotLight(gLights[3], posW, albedo.rgb, normal, viewDir), 1.0f);   // Light 3 (spot - RED)
    //return float4(ComputePointLight(gLights[4], posW, albedo.rgb, normal, viewDir), 1.0f);  // Light 4 (point - BLUE)
    
    // Check light properties
    //return float4(gLights[0].Strength, 1.0f);        // Light 0 strength (should be gray 0.3)
    //return float4(gLights[3].Strength, 1.0f);        // Light 3 strength (should be red 10,1,1)
    //return float4(gLights[4].Strength, 1.0f);        // Light 4 strength (should be blue 1,1,10)
    
    return color;
}
// AABB - bounding box
//Tangent Bitangent Normal - TBN, nMap * TBN
