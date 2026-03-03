#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "Terrain/TerrainTypes.h"

// Константы объекта: мировая матрица, инверс-транспонированная, трансформация текстуры
struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 WorldInvTranspose = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
    
    // NEW: Previous frame world matrix for velocity computation
    DirectX::XMFLOAT4X4 PrevWorld = MathHelper::Identity4x4();
};

// Константы прохода: вид, проекция, камера, время, фоновый свет, массив источников света
struct PassConstants
{
    DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
    
    // NEW: Previous frame matrices for velocity computation
    DirectX::XMFLOAT4X4 PrevView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 PrevProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 PrevViewProj = MathHelper::Identity4x4();
    
    // NEW: Jittered matrices for TAA
    DirectX::XMFLOAT4X4 ProjJittered = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProjJittered = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvViewProjJittered = MathHelper::Identity4x4();
    DirectX::XMFLOAT2 PrevViewportJitter = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 cbPad0 = { 0.0f, 0.0f };  // Padding for 16-byte alignment
    
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float cbPerObjectPad1 = 0.0f;   
    DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 InvRenderTargetSize = { 0.0f, 0.0f };
    float NearZ = 1.0f;
    float FarZ = 1000.0f;
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;

    DirectX::XMFLOAT4 AmbientLight = { 0.0f, 0.0f, 0.0f, 1.0f };

    // TAA: джиттер в NDC координатах, нормализованный к [-0.5, 0.5]
    DirectX::XMFLOAT2 ViewportJitter = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 TAAModulation = { 0.9f, 0.0f }; // x = modulation factor, y = padding

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MaxLights per object.
    Light Lights[MaxLights];
};

// Вершина меша: позиция, нормаль, UV, касательная
struct Vertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT3 Normal; 
    DirectX::XMFLOAT2 TexC;
    DirectX::XMFLOAT3 TangentU;
};

// Ресурсы на один кадр: аллокатор команд, константные буферы (Pass, Material, Object, Terrain).
// Нельзя сбрасывать аллокатор и обновлять CB, пока GPU не закончил с этим кадром.
struct FrameResource
{
public:
    
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount, UINT terrainNodeCount = 0);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB = nullptr;
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;
    
    // Константный буфер террейна: по одной записи на каждый видимый узел
    std::unique_ptr<UploadBuffer<Terrain::TerrainDrawCB>> TerrainCB = nullptr;

    // Значение fence для проверки, что GPU закончил с этим кадром
    UINT64 Fence = 0;
};