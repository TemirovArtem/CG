// DebugVelocityVisualize.hlsl - Visualize velocity buffer for debugging
// VS: Fullscreen triangle
// PS: Visualize velocity as color (red = X, green = Y, magnitude = brightness)

Texture2D gVelocityBuffer : register(t7);  // Changed from t0 to t7 to match root signature
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

// Visualize velocity as color
float4 PS(VSOutput pin) : SV_Target
{
    float2 velocity = gVelocityBuffer.Sample(gSampler, pin.TexC).xy;
    
    // Raw velocity (before scaling) for magnitude check
    float rawMagnitude = length(velocity);
    
    // Очень агрессивная визуализация для дебага
    // Даже очень малые velocity будут видны
    float scale = 200.0f;  // Очень большой масштаб
    float2 scaledVelocity = velocity * scale;
    
    float magnitude = length(scaledVelocity);
    
    // Если АБСОЛЮТНО нет движения, показываем черный
    if (rawMagnitude < 0.00000001f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);  // Черный для абсолютного нуля
    }
    
    // Если очень малое движение, показываем темно-серый
    if (rawMagnitude < 0.0001f)
    {
        return float4(0.2f, 0.2f, 0.2f, 1.0f);  // Темно-серый для очень малых velocity
    }
    
    // Для движущихся объектов используем направленные цвета
    // Вычисляем угол направления velocity
    float angle = atan2(velocity.y, velocity.x);  // [-PI, PI]
    float hue = (angle + 3.14159265f) / (2.0f * 3.14159265f);  // [0, 1]
    
    // Преобразуем hue в RGB
    float3 color;
    float h6 = hue * 6.0f;
    float x = 1.0f - abs(fmod(h6, 2.0f) - 1.0f);
    
    if (h6 < 1.0f)
        color = float3(1.0f, x, 0.0f);  // Red to Yellow
    else if (h6 < 2.0f)
        color = float3(x, 1.0f, 0.0f);  // Yellow to Green
    else if (h6 < 3.0f)
        color = float3(0.0f, 1.0f, x);  // Green to Cyan
    else if (h6 < 4.0f)
        color = float3(0.0f, x, 1.0f);  // Cyan to Blue
    else if (h6 < 5.0f)
        color = float3(x, 0.0f, 1.0f);  // Blue to Magenta
    else
        color = float3(1.0f, 0.0f, x);  // Magenta to Red
    
    // Модулируем яркость по величине velocity (очень агрессивно)
    float brightness = saturate(rawMagnitude * 500.0f);  // Очень сильное усиление
    color *= brightness;
    
    // Добавляем базовую яркость для видимости направления
    color = saturate(color + 0.3f);
    
    return float4(color, 1.0f);
}
