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
    
    float3 gEyePosW;
    float  gPad0;
    
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    
    float4 gAmbientLight;
};

// Texture bindings
Texture2D    gHeightmap : register(t0);
Texture2D    gCraterMap : register(t1);  // NEW: Crater deformation texture
Texture2D    gAlbedoMap : register(t2);  // Moved from t1
Texture2D    gNormalMap : register(t3);  // Moved from t2

SamplerState gSamLinearWrap  : register(s0);
SamplerState gSamLinearClamp : register(s1);

// Helper function to sample combined height (HeightMap + CraterMap)
// heightmapUV - local UV for heightmap texture
// globalUV - global terrain UV for CraterMap
float SampleCombinedHeight(float2 heightmapUV, float2 globalUV)
{
    float h = gHeightmap.SampleLevel(gSamLinearClamp, heightmapUV, 0).r;
    float c = gCraterMap.SampleLevel(gSamLinearClamp, globalUV, 0).r;
    return h + c;
}

// Compute terrain normal from combined height using finite differences
// heightmapUV - local UV for heightmap texture
// globalUV - global terrain UV for CraterMap
// localTexelSize - size of one texel in heightmap UV space (for local heightmap)
// terrainWorldSize - size of entire terrain in world units
float3 ComputeTerrainNormal(float2 heightmapUV, float2 globalUV, float2 localTexelSize, float terrainWorldSize)
{
    // Use a fixed step size in global UV space for consistent normal calculation
    // This should match the resolution we want for normal calculation
    // Using 1 texel of CraterMap (1024x1024) as step size
    float2 globalStep = float2(1.0f / 1024.0f, 1.0f / 1024.0f);
    
    // Calculate corresponding step in local heightmap UV space
    // We need to maintain the same world-space distance for both samples
    // localStep should represent the same world distance as globalStep
    float2 localStep = localTexelSize;
    
    // Sample heights at 4 neighboring points
    float hL = SampleCombinedHeight(heightmapUV + float2(-localStep.x, 0), globalUV + float2(-globalStep.x, 0));
    float hR = SampleCombinedHeight(heightmapUV + float2(localStep.x, 0), globalUV + float2(globalStep.x, 0));
    float hD = SampleCombinedHeight(heightmapUV + float2(0, -localStep.y), globalUV + float2(0, -globalStep.y));
    float hU = SampleCombinedHeight(heightmapUV + float2(0, localStep.y), globalUV + float2(0, globalStep.y));
    
    // Compute normal using finite differences
    // The scale factor (2.0) represents the distance between samples in world space
    float3 normal = normalize(float3(hL - hR, 2.0f, hD - hU));
    return normal;
}

struct VSInput
{
    float2 UV : TEXCOORD0; // UV сетки [0..1]
    uint VertexID : SV_VertexID; // Индекс вершины (для определения шторы)
};

struct VSOutput
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : TEXCOORD0;
    float2 TexC    : TEXCOORD1;  // Local heightmap UV
    float2 GlobalUV : TEXCOORD2; // Global terrain UV for CraterMap
};

VSOutput VS(VSInput vin)
{
    VSOutput vout;
    
    // Определяем, относится ли вершина к шторе (вершины после базовой сетки)
    int baseVertexCount = 65 * 65;   // GRID_SIZE * GRID_SIZE
    int curtainVertexCount = 65 * 4; // 4 ребра по GRID_SIZE вершин
    bool isCurtain = vin.VertexID >= baseVertexCount && vin.VertexID < (baseVertexCount + curtainVertexCount);
    
    // Преобразование UV сетки в мировые XZ
    float2 worldXZ = gNodeMinXZ + vin.UV * gNodeSize;
    
    // UV -> семплирование высоты из текстуры Height и CraterMap
    float2 heightmapUV = gHeightmapMinUV + vin.UV * gHeightmapSizeUV;
    
    // Calculate global terrain UV for CraterMap sampling
    // CraterMap covers entire terrain from -worldSize/2 to +worldSize/2
    // Convert world XZ to [0,1] UV space
    float terrainWorldSize = 1000.0f; // TERRAIN_WORLD_SIZE constant
    float2 globalTerrainUV = (worldXZ + terrainWorldSize * 0.5f) / terrainWorldSize;
    
    // Sample HeightMap with local UV, CraterMap with global UV
    float heightSample = gHeightmap.SampleLevel(gSamLinearClamp, heightmapUV, 0).r;
    float craterSample = gCraterMap.SampleLevel(gSamLinearClamp, globalTerrainUV, 0).r;
    float combinedHeight = heightSample + craterSample;
    
    // Высота в мировых координатах
    float worldY = combinedHeight * gHeightScale;
    
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
    
    // Преобразование в clip space
    vout.PosH = mul(float4(posW, 1.0f), gViewProj);
    
    // Текстурные координаты те же что и для heightmap
    vout.TexC = heightmapUV;
    
    // Global terrain UV for CraterMap
    vout.GlobalUV = globalTerrainUV;
    
    return vout;
}

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
};

GBufferOutput PS(VSOutput pin)
{
    GBufferOutput output;
    
    // Сэмпл альбедо по UV (clamp для устранения артефактов на краях)
    float4 albedo = gAlbedoMap.Sample(gSamLinearClamp, pin.TexC);
    output.Albedo = albedo;
    
    // Compute normal using screen-space derivatives (ddx/ddy)
    // This automatically handles the correct world-space distances
    float3 dPdx = ddx(pin.PosW);
    float3 dPdy = ddy(pin.PosW);
    float3 normal = normalize(cross(dPdy, dPdx));
    
    output.Normal = float4(normal, 1);
    return output;
}
