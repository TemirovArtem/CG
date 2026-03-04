// DebugVelocityVisualize.hlsl - Visualize velocity buffer for debugging
// VS: Fullscreen triangle
// PS: Show normal scene with red overlay for moving pixels

Texture2D gVelocityBuffer : register(t7);  // Changed from t0 to t7 to match root signature
Texture2D gSceneTexture : register(t5);    // Current frame scene texture (from lighting pass)
SamplerState gSampler : register(s0);

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

// Fullscreen triangle vertex shader
VSOutput VS(uint id : SV_VertexID)
{
    float2 verts[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };
    
    VSOutput vout;
    vout.PosH = float4(verts[id], 0.0, 1.0);
    vout.TexC = 0.5f * (verts[id] + 1.0f);
    vout.TexC.y = 1.0f - vout.TexC.y;
    
    return vout;
}

// Visualize velocity as red overlay on normal scene
float4 PS(VSOutput pin) : SV_Target
{
    // Sample velocity
    float2 velocity = gVelocityBuffer.Sample(gSampler, pin.TexC).xy;
    
    // Sample scene color
    float3 sceneColor = gSceneTexture.Sample(gSampler, pin.TexC).rgb;
    
    // Calculate velocity magnitude
    float velocityMag = length(velocity);
    
    // Threshold for "moving" pixels (adjust as needed)
    float threshold = 0.0001f;
    
    // If velocity is above threshold, blend with red
    if (velocityMag > threshold)
    {
        // Scale velocity magnitude for visualization
        float intensity = saturate(velocityMag * 100.0f);
        
        // Blend scene color with red based on velocity magnitude
        // More velocity = more red
        float3 redOverlay = float3(1.0f, 0.0f, 0.0f);
        sceneColor = lerp(sceneColor, redOverlay, intensity * 0.7f);
    }
    
    return float4(sceneColor, 1.0f);
}
