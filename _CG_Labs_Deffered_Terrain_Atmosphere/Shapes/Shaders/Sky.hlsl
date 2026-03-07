//=============================================================================
// Sky.hlsl - Sky rendering with atmospheric scattering
// Based on GPU Gems 2, Chapter 16: Accurate Atmospheric Scattering
//=============================================================================

#define PI 3.14159265359

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    uint gMaterialIndex;
    uint gObjPad0;
    uint gObjPad1;
    uint gObjPad2;
};

cbuffer cbPass : register(b2) // Изменено с b1 на b2
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
};

// Atmosphere parameters
cbuffer cbAtmosphere : register(b3) // Изменено с b2 на b3
{
    float3 gSunDirection;
    float gSunIntensity;
    
    float3 gRayleighScattering;
    float gPlanetRadius;
    
    float3 gMieScattering;
    float gAtmosphereRadius;
    
    float gRayleighScaleHeight;
    float gMieScaleHeight;
    float gMieAnisotropy;
    float gAtmosphereDensity;
    
    float3 gCameraPositionKm;
    float gExposure;
    
    int gNumSamples;
    int gNumLightSamples;
    float2 gAtmoPad;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosL : POSITION;
};

// Rayleigh phase function
// P_r(θ) = (3/(16π)) * (1 + cos² θ)
float RayleighPhase(float cosTheta)
{
    return (3.0 / (16.0 * PI)) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein phase function for Mie scattering
// P_m(θ, g) = (1 - g²) / [4π * (1 + g² - 2g*cosθ)^(3/2)]
float MiePhase(float cosTheta, float g)
{
    float g2 = g * g; // ~ 0.5776
    float num = (1.0 - g2); // ~ 0.4224, числитель
    float denom = 4.0 * PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5); // , значменатель
    return num / max(denom, 0.0001);
}

// Compute atmospheric scattering
float3 ComputeAtmosphericScattering(float3 rayDir)
{
    float3 sunDir = normalize(gSunDirection); // направение на солнце
    float cosTheta = dot(rayDir, sunDir); // косинус угла между "взглядом" камеры и солнцем
    
    // View angle from horizon
    float viewY = rayDir.y;
    
    // Sun elevation: еси плюс то day, если минус то night
    float sunElevation = sunDir.y;
    
    // Night factor: 0 = full day, 1 = full night
    // Transition happens between sunElevation -0.1 (civil twilight) and -0.3 (night)
    float nightFactor = saturate((-sunElevation - 0.05) * 5.0);
    
    // Day factor for scaling all daytime lighting
    float dayFactor = 1.0 - nightFactor;
    
    // Optical depth - increases as we look toward horizon
    float zenithAngle = acos(max(viewY, 0.001));
    float opticalDepthRayleigh = 1 / max(cos(zenithAngle), 0.035); // формула чепмена
    opticalDepthRayleigh = min(opticalDepthRayleigh, 40.0);
    
    // Mie optical depth (affected by density/pollution)
    float opticalDepthMie = opticalDepthRayleigh * gAtmosphereDensity * 0.1;
    
    // Rayleigh scattering coefficients at sea level
    float3 betaR = float3(5.8e-3, 13.5e-3, 33.1e-3);
    
    // Mie scattering coefficient (haze/pollution)
    float3 betaM = float3(21e-3, 21e-3, 21e-3) * gAtmosphereDensity;
    
    // Phase functions
    float phaseR = RayleighPhase(cosTheta);
    float phaseM = MiePhase(cosTheta, gMieAnisotropy);
    
    // Extinction
    float3 extinction = exp(-(betaR * opticalDepthRayleigh + betaM * opticalDepthMie));
    
    // Calculate sun optical depth based on elevation angle
    float sunZenithCos = max(sunElevation, 0.001);
    float sunOpticalDepth;
    
    if (sunElevation > 0.0)
    {
        sunOpticalDepth = 1.0 / (sunZenithCos + 0.15 * pow(max(93.885 - degrees(acos(sunZenithCos)), 0.1), -1.253));
    }
    else
    {
        // Sun below horizon - exponentially increase optical depth
        sunOpticalDepth = 40.0;
    }
    sunOpticalDepth = min(sunOpticalDepth, 40.0);
    
    // Sun transmittance - goes to zero as sun sets
    float3 sunTransmittance = exp(-(betaR * sunOpticalDepth * 1.5 + betaM * sunOpticalDepth * 0.15));
    sunTransmittance *= dayFactor; // Fade out completely at night
    
    // Sunset factor for color tinting (only during twilight, not at night)
    float sunsetFactor = saturate(1.0 - sunElevation * 3.0) * dayFactor;
    sunsetFactor = sunsetFactor * sunsetFactor;
    
    // Boost red/orange at sunset
    float3 sunsetBoost = float3(1.5, 1.1, 0.7);
    float3 tintedTransmittance = sunTransmittance * lerp(float3(1.0, 1.0, 1.0), sunsetBoost, sunsetFactor);
    
    // In-scattering (only when there's sunlight)
    float3 rayleighScatter = betaR * phaseR * (1.0 - extinction);
    float3 mieScatter = betaM * phaseM * (1.0 - exp(-opticalDepthMie));
    
    float3 sunColor = tintedTransmittance * gSunIntensity;
    float3 inscatter = (rayleighScatter + mieScatter) * sunColor;
    
    // Warm horizon glow during sunset (fades at night)
    float horizonGlow = exp(-abs(viewY) * 3.0) * sunsetFactor * dayFactor;
    float3 warmGlow = float3(1.0, 0.4, 0.1) * horizonGlow * gSunIntensity * 0.15;
    inscatter += warmGlow * tintedTransmittance;
    
    // Twilight glow at horizon when sun is just below horizon
    float twilightFactor = saturate(1.0 - abs(sunElevation + 0.1) * 10.0) * saturate(-sunElevation * 10.0);
    float twilightHorizon = exp(-abs(viewY) * 2.0) * twilightFactor;
    float3 twilightColor = float3(0.3, 0.15, 0.1) * twilightHorizon * gSunIntensity * 0.1;
    inscatter += twilightColor;
    
    // Night sky color
    float3 nightSky = float3(0.005, 0.007, 0.015); // Dark blue night sky
    
    // Add slight gradient - darker at zenith, slightly lighter at horizon
    float nightHorizonBrightness = exp(-abs(viewY) * 2.0) * 0.5 + 0.5;
    nightSky *= nightHorizonBrightness;
    
    // Daytime ambient sky
    float ambientScale = lerp(0.1, 0.03, sunsetFactor);
    float3 daySky = float3(0.05, 0.1, 0.2) * (1.0 - extinction) * gSunIntensity * ambientScale;
    
    // Blend between day and night sky
    float3 ambientSky = lerp(daySky, nightSky, nightFactor);
    
    float3 skyColor = inscatter + ambientSky;
    
    // Ground color when looking down
    if (viewY < 0.0)
    {
        float groundFade = saturate(-viewY * 3.0);
        float3 dayGround = float3(0.4, 0.35, 0.3) * tintedTransmittance * gSunIntensity * 0.05;
        float3 nightGround = float3(0.01, 0.01, 0.015);
        float3 groundColor = lerp(dayGround, nightGround, nightFactor);
        skyColor = lerp(skyColor, groundColor, groundFade);
    }
    
    return skyColor;
}

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    
    vout.PosL = vin.PosL;
    
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    posW.xyz += gEyePosW;
    
    // Set z = w so that z/w = 1 (skydome always on far plane)
    vout.PosH = mul(posW, gViewProj).xyww;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float3 rayDir = normalize(pin.PosL);
    
    // Compute atmospheric scattering
    float3 color = ComputeAtmosphericScattering(rayDir);
    
    // Add sun disk
    float3 sunDir = normalize(gSunDirection);
    float sunDot = dot(rayDir, sunDir);
    float sunElevation = sunDir.y;
    
    // Night factor - sun disappears when below horizon
    float nightFactor = saturate((-sunElevation - 0.05) * 5.0);
    float dayFactor = 1.0 - nightFactor;
    
    // Only show sun when above horizon
    if (sunElevation > -0.05)
    {
        // Sunset factor for color tinting
        float sunsetFactor = saturate(1.0 - sunElevation * 3.0) * dayFactor;
        sunsetFactor = sunsetFactor * sunsetFactor;
        
        // Sun disk color - white/yellow at noon, orange/red at sunset
        float3 sunDiskColor = lerp(float3(1.0, 0.98, 0.95), float3(1.0, 0.5, 0.2), sunsetFactor);
        
        // Sun disk - fade out as it sets
        float sunDisk = smoothstep(0.9997, 0.9999, sunDot);
        float3 sunColor = sunDiskColor * gSunIntensity * 0.15 * sunDisk * dayFactor;
        
        // Sun corona/glow - also tinted at sunset and fades at night
        float3 glowTint = lerp(float3(1.0, 0.9, 0.7), float3(1.0, 0.6, 0.3), sunsetFactor);
        float sunGlow = pow(max(sunDot, 0.0), 512.0) * gSunIntensity * 0.3;
        sunGlow += pow(max(sunDot, 0.0), 64.0) * gSunIntensity * 0.05;
        float3 glowColor = glowTint * sunGlow * dayFactor;
        
        color += sunColor + glowColor;
    }
    
    // Tone mapping (Reinhard)
    color = color / (color + 1.0);
    
    // Exposure adjustment
    color = 1.0 - exp(-gExposure * color * 2.0);
    
    // Gamma correction
    color = pow(max(color, 0.0), 1.0 / 2.2);
    
    return float4(color, 1.0);
}
