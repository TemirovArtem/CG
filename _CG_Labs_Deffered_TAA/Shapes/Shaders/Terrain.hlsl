// VS: по UV сетки и карте высот вычисляет мировую позицию и нормаль.
// PS: записывает альбедо и нормаль в два целевых буфера (MRT).



cbuffer TerrainDrawCB : register(b0)
{
    float2 gNodeMinXZ;       // Минимальный угол тайла в мировых координатах (XZ)
    float  gNodeSize;        // Размер стороны тайла в мировых единицах
    int    gLOD;             // Уровень детализации (0=низкий, 2=высокий)
    float  gHeightScale;     // Максимальная высота в мировых единицах
    int    gHeightmapIndex;  // Индекс карты высот в массиве
    int    gEnableCurtains;  // Вкл/выкл шторы
    float  gCurtainHeight;   
    float2 gHeightmapMinUV;  // Смещение UV в карте высот для этого тайла
    float2 gHeightmapSizeUV; // Масштаб UV для области тайла в карте высот
    int    gCurtainEdges;    // Битмаска рёбер с занавесами: 0=лево, 1=право, 2=верх, 3=низ
    float  gCurtainExtension; // Горизонтальное расширение занавеса
    float2 gPadding2;        
};


cbuffer PassConstants : register(b1)
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
    float  gPad1;
    
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
};

//

Texture2D    gHeightmap : register(t0);
Texture2D    gAlbedoMap : register(t1);
Texture2D    gNormalMap : register(t2);

SamplerState gSamLinearWrap  : register(s0);
SamplerState gSamLinearClamp : register(s1);

//

struct VSInput
{
    float2 UV : TEXCOORD0; // UV сетки [0..1]
    uint VertexID : SV_VertexID; // Индекс вершины (для определения шторы)
};

struct VSOutput
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : TEXCOORD0;
    float2 TexC    : TEXCOORD1;
    
    // Positions for velocity computation
    float4 CurrentPosClip : TEXCOORD2;   // Current position without jitter
    float4 PrevPosClip : TEXCOORD3;      // Previous frame position (same as current for static terrain)
};

VSOutput VS(VSInput vin)
{
    VSOutput vout;
    
    // Определяем, относится ли вершина к шторе (вершины после базовой сетки)
    int baseVertexCount = 65 * 65;   // GRID_SIZE * GRID_SIZE
    int curtainVertexCount = 65 * 4; // 4 ребра по GRID_SIZE вершин
    bool isCurtain = vin.VertexID >= baseVertexCount && vin.VertexID < (baseVertexCount + curtainVertexCount);
    //
    
    // Преобразование UV сетки в мировые XZ
    float2 worldXZ = gNodeMinXZ + vin.UV * gNodeSize;
    
    // UV -> семплирование высоты из текстуры Height
    float2 heightmapUV = gHeightmapMinUV + vin.UV * gHeightmapSizeUV;
    float heightSample = gHeightmap.SampleLevel(gSamLinearClamp, heightmapUV, 0).r;
    
    // Высота в мировых координатах
    float worldY = heightSample * gHeightScale;
    
    // Для вершин штор: опускаем вниз и смещаем по горизонтали, чтобы закрыть щели
    if (isCurtain && gEnableCurtains)
    {
        worldY -= gCurtainHeight;
        
        int curtainIndex = vin.VertexID - baseVertexCount;
        
        if (curtainIndex < 65)        // Левая штора
            worldXZ.x -= gCurtainExtension;
        else if (curtainIndex < 130)  // Правая штора
            worldXZ.x += gCurtainExtension;
        else if (curtainIndex < 195)  // Нижняя штора
            worldXZ.y -= gCurtainExtension;
        else                          // Верхняя штора
            worldXZ.y += gCurtainExtension;
    }
    
    // Итоговая позиция
    float3 posW = float3(worldXZ.x, worldY, worldXZ.y);
    vout.PosW = posW;
    
    // Compute current position without jitter for velocity
    float4 currentClip = mul(float4(posW, 1.0f), gViewProj);
    vout.CurrentPosClip = currentClip;
    
    // FIXED: Use jittered ViewProj for rasterization (TAA)
    vout.PosH = mul(float4(posW, 1.0f), gViewProjJittered);
    
    // For static terrain, previous position = current position
    // Terrain doesn't move, so velocity should be zero (camera motion only)
    vout.PrevPosClip = mul(float4(posW, 1.0f), gPrevViewProj);
    
    // Текстурные координаты те же что и для heightmap
    vout.TexC = heightmapUV;
    
    return vout;
}

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float2 Velocity : SV_Target2;  // Velocity buffer output
};

// Compute velocity with numerical stability checks (same as GeometryTAA.hlsl)
float2 ComputeVelocity(float4 currentClip, float4 prevClip)
{
    const float EPSILON = 0.0001f;
    const float MAX_VELOCITY = 0.5f; // Clamp to 50% of screen
    
    // Check for invalid w components
    if (abs(currentClip.w) < EPSILON || abs(prevClip.w) < EPSILON)
    {
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

GBufferOutput PS(VSOutput pin)
{
    GBufferOutput output;
    
    // Сэмпл альбедо и нормали по одним и тем же UV (clamp для устранения артефактов на краях)
    float4 albedo = gAlbedoMap.Sample(gSamLinearClamp, pin.TexC);
    output.Albedo = albedo;
    
    float3 normal = gNormalMap.Sample(gSamLinearClamp, pin.TexC).rgb;
    normal = normalize(normal * 2.0f - 1.0f);
    output.Normal = float4(normal, 1);
    
    // Compute and write velocity
    // For static terrain, velocity comes only from camera motion
    output.Velocity = ComputeVelocity(pin.CurrentPosClip, pin.PrevPosClip);
    
    return output;
}
