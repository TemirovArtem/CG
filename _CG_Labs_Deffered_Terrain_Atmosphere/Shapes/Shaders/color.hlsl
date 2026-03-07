// color.hlsl — простой шейдер: трансформация в clip space и передача цвета вершины в PS.
 
cbuffer cbPerObject : register(b0)
{
	float4x4 gWorld; 
};

cbuffer cbPass : register(b1)
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

struct VertexIn
{
	float3 PosL  : POSITION;
    float4 Color : COLOR;
};

struct VertexOut
{
	float4 PosH  : SV_POSITION;
    float4 Color : COLOR;
};

// VS: мировая позиция и view-proj; цвет вершины передаётся в PS
VertexOut VS(VertexIn vin)
{
	VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = mul(posW, gViewProj);
    vout.Color = vin.Color;
    return vout;
}

// PS: выходной цвет = интерполированный цвет вершины
float4 PS(VertexOut pin) : SV_Target
{
    return pin.Color;
}


