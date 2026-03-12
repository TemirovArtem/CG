// Шейдер для теней с поддержкой альфа-теней

// Глобальные ресурсы
RaytracingAccelerationStructure gScene : register(t0, space0);
Texture2D<float4> gPositionMap : register(t1, space0);
Texture2D<float4> gNormalMap : register(t2, space0);

// Массив текстур для альфа-тестирования
Texture2D gTextures[] : register(t0, space1);

// Буферы геометрии для всех объектов
struct DxrVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float3 Tangent;
};

ByteAddressBuffer gVerticesRaw : register(t0, space2);
ByteAddressBuffer gIndices : register(t0, space3);

// Загрузка вершины из буфера (размер вершины 44 байта)
DxrVertex LoadVertex(uint index)
{
    uint offset = index * 44;
    
    DxrVertex v;
    v.Position.x = asfloat(gVerticesRaw.Load(offset + 0));
    v.Position.y = asfloat(gVerticesRaw.Load(offset + 4));
    v.Position.z = asfloat(gVerticesRaw.Load(offset + 8));
    
    v.Normal.x = asfloat(gVerticesRaw.Load(offset + 12));
    v.Normal.y = asfloat(gVerticesRaw.Load(offset + 16));
    v.Normal.z = asfloat(gVerticesRaw.Load(offset + 20));
    
    v.TexCoord.x = asfloat(gVerticesRaw.Load(offset + 24));
    v.TexCoord.y = asfloat(gVerticesRaw.Load(offset + 28));
    
    v.Tangent.x = asfloat(gVerticesRaw.Load(offset + 32));
    v.Tangent.y = asfloat(gVerticesRaw.Load(offset + 36));
    v.Tangent.z = asfloat(gVerticesRaw.Load(offset + 40));
    
    return v;
}

SamplerState gSampler : register(s0);

// Выходная маска теней
RWTexture2D<float> gShadowMask : register(u0);

// Константы для расчета теней
cbuffer cbDxrShadow : register(b0, space0)
{
    float3 gLightDirW;
    float gConeAngleRad;
    uint gSampleCount;
    uint gFrameIndex;
    uint gDownscale;
    uint _pad0;
    float gMaxDistance;
    float gNormalBias;
    float2 _pad1;
};

// Локальные константы для каждого объекта (не работают в Any-Hit, используем хардкод)
cbuffer LocalRootConstants : register(b0, space1)
{
    uint gTextureIndex;
    uint gVertexOffset;
    uint gIndexOffset;
    uint _localPad;
};

// Данные луча тени
struct ShadowPayload
{
    bool isVisible; // true = освещено, false = в тени
};

// Хеш-функция для случайного сэмплирования
float Hash21(float2 p, uint frame)
{
    float h = dot(p, float2(12.9898, 78.233)) + (float) frame * 0.6180339887;
    return frac(sin(h) * 43758.5453);
}

void BuildBasis(float3 n, out float3 t, out float3 b)
{
    float3 up = (abs(n.y) < 0.999f) ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

float3 JitterConeDir(float3 dir, float2 xi, float coneAngle)
{
    if (coneAngle <= 0.0f)
        return dir;
    
    float3 t, b;
    BuildBasis(dir, t, b);
    
    float r = sqrt(xi.x);
    float phi = 6.28318530718f * xi.y;
    float2 disk = r * float2(cos(phi), sin(phi));
    
    float k = tan(coneAngle);
    float3 d = normalize(dir + t * (disk.x * k) + b * (disk.y * k));
    return d;
}

// Генерация лучей для теней
[shader("raygeneration")]
void ShadowRayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    // Получаем размеры G-буфера
    uint fw, fh;
    gPositionMap.GetDimensions(fw, fh);
    
    // Вычисляем пиксель с учетом downscale
    uint s = max(gDownscale, 1u);
    uint2 fullPix = uint2(
        min(launchIndex.x * s + s / 2, fw - 1),
        min(launchIndex.y * s + s / 2, fh - 1)
    );
    
    // Читаем данные из G-буфера
    float3 posW = gPositionMap.Load(int3(fullPix, 0)).xyz;
    float3 nrm = gNormalMap.Load(int3(fullPix, 0)).xyz;
    
    // Если фон - сразу освещено
    if (dot(nrm, nrm) < 1e-8f)
    {
        gShadowMask[launchIndex] = 1.0f;
        return;
    }
    
    float3 N = normalize(nrm);
    float3 dirToLight = normalize(-gLightDirW);
    float3 origin = posW + N * gNormalBias;
    float tMax = max(gMaxDistance, 0.0f);
    
    uint samples = max(gSampleCount, 1u);
    float visible = 0.0f;
    
    for (uint i = 0; i < samples; ++i)
    {
        float2 seed = float2(launchIndex) + float2(i * 17.0f, i * 29.0f);
        float u1 = Hash21(seed + 0.13f, gFrameIndex);
        float u2 = Hash21(seed + 0.73f, gFrameIndex);
        
        float3 rayDir = JitterConeDir(dirToLight, float2(u1, u2), gConeAngleRad);
        
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = rayDir;
        ray.TMin = 0.001f;
        ray.TMax = tMax;
        
        ShadowPayload payload;
        payload.isVisible = true;
        
        // Трассируем луч к источнику света
        // Any-Hit шейдер проверит альфа-канал для прозрачных объектов
        TraceRay(gScene,
            RAY_FLAG_NONE,
            0xFF,
            0,
            0,
            0,
            ray,
            payload);
        
        visible += payload.isVisible ? 1.0f : 0.0f;
    }
    
    gShadowMask[launchIndex] = visible / (float) samples;
}

// Any-Hit шейдер для проверки альфа-канала
[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    
    // Офсеты для каждого объекта в unified буферах
    
    uint vertexOffset = 0;
    uint indexOffset = 0;
    uint textureIndex = 0;
    
    if (instanceID == 0)
    {
        vertexOffset = 0;
        indexOffset = 0;
        textureIndex = 2;
    }
    else if (instanceID == 1)
    {
        vertexOffset = 7476;
        indexOffset = 7476;
        textureIndex = 10;
    }
    else if (instanceID == 2) // alphaPlane с прозрачной текстурой
    {
        vertexOffset = 9852;
        indexOffset = 21282;
        textureIndex = 17-1;
    }
    
    // Вычисляем позицию в индексном буфере
    uint baseIndex = indexOffset + primitiveIndex * 3;
    
    // Читаем индексы треугольника
    uint i0 = gIndices.Load((baseIndex + 0) * 4);
    uint i1 = gIndices.Load((baseIndex + 1) * 4);
    uint i2 = gIndices.Load((baseIndex + 2) * 4);
    
    // Загружаем вершины
    DxrVertex v0 = LoadVertex(i0 + vertexOffset);
    DxrVertex v1 = LoadVertex(i1 + vertexOffset);
    DxrVertex v2 = LoadVertex(i2 + vertexOffset);
    
    // Интерполируем UV координаты
    float2 bary = attr.barycentrics;
    float2 uv = v0.TexCoord * (1.0f - bary.x - bary.y) + v1.TexCoord * bary.x + v2.TexCoord * bary.y;
    
    // Переворачиваем V координату
    uv.y = 1.0f - uv.y;
    
    // Wrap в диапазон 0-1
    uv = frac(uv);
    
    // Проверяем альфа-канал текстуры
    float alpha = gTextures[textureIndex].SampleLevel(gSampler, uv, 0).a;
    
    // Если прозрачно - игнорируем попадание
    if (alpha < 0.1f)
    {
        IgnoreHit();
    }
}

// Miss шейдер - луч дошел до света
[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.isVisible = true;
}

// Closest-hit шейдер - луч попал в геометрию
[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.isVisible = false;
}
