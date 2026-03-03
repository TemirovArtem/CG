//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#define ROOT_SIG "CBV(b0), \
                  RootConstants(b1, num32bitconstants=2), \
                  SRV(t0), \
                  SRV(t1), \
                  SRV(t2), \
                  SRV(t3), \
                  DescriptorTable(SRV(t4, numDescriptors=3, flags=DESCRIPTORS_VOLATILE)), \
                  StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"

struct Constants
{
    float4x4 World;
    float4x4 WorldView;
    float4x4 WorldViewProj;
    uint     DrawMeshlets;
    float    Time;
    float    AnimationAmplitude;
    float    AnimationFrequency;
};

struct MeshInfo
{
    uint IndexBytes;
    uint MeshletOffset;
    uint VertexStride;  // Add vertex stride info
    uint HasTexCoords;  // Flag to indicate if model has UVs
};

struct VertexAttributes
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
};

struct VertexOut
{
    float4 PositionHS   : SV_Position;
    float3 PositionVS   : POSITION0;
    float3 PositionWS   : POSITION1;
    float3 Normal       : NORMAL0;
    float2 TexCoord     : TEXCOORD0;
    uint   MeshletIndex : COLOR0;
};

struct Meshlet
{
    uint VertCount;
    uint VertOffset;
    uint PrimCount;
    uint PrimOffset;
};

ConstantBuffer<Constants> Globals             : register(b0);
ConstantBuffer<MeshInfo>  MeshInfoCB          : register(b1);

ByteAddressBuffer         VertexBuffer        : register(t0);
StructuredBuffer<Meshlet> Meshlets            : register(t1);
ByteAddressBuffer         UniqueVertexIndices : register(t2);
StructuredBuffer<uint>    PrimitiveIndices    : register(t3);


/////
// Data Loaders

VertexAttributes LoadVertexAttributes(uint vertexIndex)
{
    VertexAttributes v;
    
    // Vertex stride: Position(12) + Normal(12) + TexCoord(8) = 32 bytes
    uint stride = 32;
    uint offset = vertexIndex * stride;
    
    // Load Position (float3)
    v.Position = asfloat(VertexBuffer.Load3(offset));
    offset += 12;
    
    // Load Normal (float3)
    v.Normal = asfloat(VertexBuffer.Load3(offset));
    offset += 12;
    
    // Load TexCoord (float2) - model now has UV coordinates!
    v.TexCoord = asfloat(VertexBuffer.Load2(offset));
    
    return v;
}

uint3 UnpackPrimitive(uint primitive)
{
    // Unpacks a 10 bits per index triangle from a 32-bit uint.
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

uint3 GetPrimitive(Meshlet m, uint index)
{
    return UnpackPrimitive(PrimitiveIndices[m.PrimOffset + index]);
}

uint GetVertexIndex(Meshlet m, uint localIndex)
{
    localIndex = m.VertOffset + localIndex;

    if (MeshInfoCB.IndexBytes == 4) // 32-bit Vertex Indices
    {
        return UniqueVertexIndices.Load(localIndex * 4);
    }
    else // 16-bit Vertex Indices
    {
        // Byte address must be 4-byte aligned.
        uint wordOffset = (localIndex & 0x1);
        uint byteOffset = (localIndex / 2) * 4;

        // Grab the pair of 16-bit indices, shift & mask off proper 16-bits.
        uint indexPair = UniqueVertexIndices.Load(byteOffset);
        uint index = (indexPair >> (wordOffset * 16)) & 0xffff;

        return index;
    }
}

VertexOut GetVertexAttributes(uint meshletIndex, uint vertexIndex)
{
    VertexAttributes v = LoadVertexAttributes(vertexIndex);

    // Фрактальное дыхание: используем синусоиду с учетом позиции вершины
    // Создаем фрактальный эффект, комбинируя несколько частот
    float3 worldPos = v.Position;
    
    // Базовая волна
    float wave1 = sin(Globals.Time * Globals.AnimationFrequency * 6.28318530718); // 2*PI
    
    // Добавляем фрактальные компоненты на основе позиции
    float fractalFactor = sin(worldPos.x * 0.1 + Globals.Time * Globals.AnimationFrequency * 3.14159265359) * 0.5 +
                          sin(worldPos.y * 0.1 + Globals.Time * Globals.AnimationFrequency * 4.71238898038) * 0.3 +
                          sin(worldPos.z * 0.1 + Globals.Time * Globals.AnimationFrequency * 6.28318530718) * 0.2;
    
    // Комбинируем волны для создания дыхательного эффекта
    float breathingScale = 1.0 + (wave1 * 0.5 + fractalFactor * 0.5) * Globals.AnimationAmplitude * 0.01;
    
    // Применяем масштабирование от центра
    float3 animatedPosition = v.Position * breathingScale;

    VertexOut vout;
    vout.PositionVS = mul(float4(animatedPosition, 1), Globals.WorldView).xyz;
    vout.PositionHS = mul(float4(animatedPosition, 1), Globals.WorldViewProj);
    vout.PositionWS = mul(float4(animatedPosition, 1), Globals.World).xyz;
    vout.Normal = mul(float4(v.Normal, 0), Globals.World).xyz;
    
    // Use real UV coordinates from model
    // Try with and without flip to see which works better
    vout.TexCoord = float2(v.TexCoord.x, 1.0 - v.TexCoord.y); // Flipped V
    // vout.TexCoord = v.TexCoord; // Or without flip
    
    vout.MeshletIndex = meshletIndex;

    return vout;
}


[RootSignature(ROOT_SIG)]
[NumThreads(128, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out indices uint3 tris[126],
    out vertices VertexOut verts[64]
)
{
    Meshlet m = Meshlets[MeshInfoCB.MeshletOffset + gid];

    SetMeshOutputCounts(m.VertCount, m.PrimCount);

    if (gtid < m.PrimCount)
    {
        tris[gtid] = GetPrimitive(m, gtid);
    }

    if (gtid < m.VertCount)
    {
        uint vertexIndex = GetVertexIndex(m, gtid);
        verts[gtid] = GetVertexAttributes(gid, vertexIndex);
    }
}
