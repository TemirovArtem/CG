// TAAResolve.hlsl - Temporal Anti-Aliasing resolve pass
// VS: Fullscreen triangle vertex shader (3 вершины для покрытия экрана)
// PS: Смешивание текущего и исторического кадров

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

cbuffer PassConstants : register(b2)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    
    // Previous frame matrices for velocity computation
    float4x4 gPrevView;
    float4x4 gPrevProj;
    float4x4 gPrevViewProj;
    
    // Jittered matrices for TAA
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
    
    float2 gViewportJitter;
    float2 gTAAModulation; // x = modulation factor, y = padding
    
    Light gLights[MaxLights];
};

Texture2D gCurrentFrame : register(t5);  // Текущий кадр (из lighting pass)
Texture2D gHistoryFrame : register(t6);  // Исторический кадр
Texture2D gVelocityBuffer : register(t7);  // NEW: Velocity buffer for reprojection
SamplerState gSampler : register(s0);

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

// Fullscreen triangle vertex shader
// Генерирует 3 вершины для покрытия всего экрана
VSOutput VS(uint id : SV_VertexID)
{
    float2 verts[3] =
    {
        float2(-1.0, -1.0),  // Левый нижний угол
        float2(-1.0, 3.0),   // Левый верхний угол (за пределами экрана)
        float2(3.0, -1.0)    // Правый нижний угол (за пределами экрана)
    };
    
    VSOutput vout;
    vout.PosH = float4(verts[id], 0.0, 1.0);
    
    // Преобразование из clip space [-1, 1] в texture coordinates [0, 1]
    vout.TexC = 0.5f * (verts[id] + 1.0f);
    // Инвертируем Y координату
    vout.TexC.y = 1.0f - vout.TexC.y;
    
    return vout;

    
}

// Compute neighborhood color clamping box (min/max RGB)
// Uses 4-tap cross pattern to reduce ghosting artifacts
void ComputeNeighborhoodClamping(float2 uv, out float3 boxMin, out float3 boxMax)
{
    // Sample center and 4 neighbors (cross pattern)
    float3 center = gCurrentFrame.Sample(gSampler, uv).rgb;
    float3 neighbor0 = gCurrentFrame.Sample(gSampler, uv, int2(1, 0)).rgb;
    float3 neighbor1 = gCurrentFrame.Sample(gSampler, uv, int2(0, 1)).rgb;
    float3 neighbor2 = gCurrentFrame.Sample(gSampler, uv, int2(-1, 0)).rgb;
    float3 neighbor3 = gCurrentFrame.Sample(gSampler, uv, int2(0, -1)).rgb;
    
    // Compute min and max across all samples
    boxMin = min(center, min(neighbor0, min(neighbor1, min(neighbor2, neighbor3))));
    boxMax = max(center, max(neighbor0, max(neighbor1, max(neighbor2, neighbor3))));
}

// TAA resolve pixel shader with velocity-based reprojection
// Implements color clamping to reduce ghosting artifacts
float4 PS(VSOutput pin) : SV_Target
{
    // Sample velocity from velocity buffer (point sampling for exact pixel)
    float2 velocity = gVelocityBuffer.Sample(gSampler, pin.TexC).xy;
    
    // Compute history UV by subtracting velocity (reprojection)
    float2 historyUV = pin.TexC - velocity;
    
    // Sample current frame color
    float4 currentColor = gCurrentFrame.Sample(gSampler, pin.TexC);
    
    // Check if history UV is within valid bounds [0, 1]
    bool validHistory = all(historyUV >= 0.0f) && all(historyUV <= 1.0f);
    
    if (!validHistory)
    {
        // History is out of bounds, use current frame only
        return currentColor;
    }
    
    // Sample history frame at reprojected UV (linear sampling for smooth interpolation)
    float4 historyColor = gHistoryFrame.Sample(gSampler, historyUV);
    
    // Compute neighborhood clamping box to reduce ghosting
    float3 boxMin, boxMax;
    ComputeNeighborhoodClamping(pin.TexC, boxMin, boxMax);
    
    // Clamp history color to neighborhood box
    float3 clampedHistory = clamp(historyColor.rgb, boxMin, boxMax);
    
    // Blend current and clamped history using modulation factor
    // Higher modulation = more history influence = smoother but more ghosting
    // Lower modulation = more current frame = less ghosting but more aliasing
    float modulationFactor = gTAAModulation.x;
    float3 finalColor = lerp(currentColor.rgb, clampedHistory, modulationFactor);
    
    return float4(finalColor, 1.0f);
}

