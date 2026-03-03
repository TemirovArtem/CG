// DebugVelocity.hlsl - Визуализация velocity buffer для отладки

Texture2D gVelocityBuffer : register(t0);
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

// Визуализация velocity как цвет
// Серый = нет движения
// Красный/Зеленый = движение по X/Y
float4 PS(VSOutput pin) : SV_Target
{
    // Читаем velocity (диапазон примерно [-0.5, 0.5] в UV space)
    float2 velocity = gVelocityBuffer.Sample(gSampler, pin.TexC).xy;
    
    // Масштабируем для визуализации (увеличиваем чувствительность)
    float scale = 10.0f;  // Увеличиваем малые движения
    velocity *= scale;
    
    // Преобразуем в цвет:
    // R = движение вправо (положительное X)
    // G = движение вниз (положительное Y)
    // B = величина движения
    float3 color;
    color.r = saturate(velocity.x * 0.5f + 0.5f);  // [-1,1] -> [0,1]
    color.g = saturate(velocity.y * 0.5f + 0.5f);
    color.b = saturate(length(velocity));
    
    // Если нет движения, показываем серый (0.5, 0.5, 0.5)
    // Если есть движение, показываем цветной
    
    return float4(color, 1.0f);
}
