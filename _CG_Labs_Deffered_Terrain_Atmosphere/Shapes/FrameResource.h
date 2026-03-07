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
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float cbPerObjectPad1 = 0.0f;   
    DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 InvRenderTargetSize = { 0.0f, 0.0f };
    float NearZ = 1.0f;
    float FarZ = 1000.0f;
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;

    DirectX::XMFLOAT4 AmbientLight = { 0.0f, 0.0f, 0.0f, 1.0f };

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MaxLights per object.
    Light Lights[MaxLights];
};

// Atmospheric scattering parameters based on GPU Gems 2 Chapter 16
struct AtmosphereConstants
{
    DirectX::XMFLOAT3 SunDirection = { 0.0f, 1.0f, 0.0f };
    float SunIntensity = 22.0f;
    
    DirectX::XMFLOAT3 RayleighScattering = { 5.8e-6f, 13.5e-6f, 33.1e-6f }; // Wavelength-dependent
    float PlanetRadius = 6371000.0f; // Earth radius in meters
    
    DirectX::XMFLOAT3 MieScattering = { 21e-6f, 21e-6f, 21e-6f }; // Wavelength-independent
    float AtmosphereRadius = 6471000.0f; // Atmosphere top radius
    
    float RayleighScaleHeight = 8500.0f;  // Scale height for Rayleigh scattering
    float MieScaleHeight = 1200.0f;       // Scale height for Mie scattering
    float MieAnisotropy = 0.758f;         // Mie phase function anisotropy (g parameter)
    float AtmosphereDensity = 1.0f;       // Density multiplier (1.0 = clean, >1.0 = dirty/polluted)
    
    DirectX::XMFLOAT3 CameraPositionKm = { 0.0f, 0.0f, 0.0f }; // Camera position in km
    float Exposure = 2.0f;                // HDR exposure
    
    int NumSamples = 16;                  // Number of samples along view ray
    int NumLightSamples = 8;              // Number of samples along light ray
    int EnableAtmosphere = 1;             // Enable/disable atmospheric scattering (0 = off, 1 = on)
    float pad1;
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
    
    // Константный буфер атмосферы
    std::unique_ptr<UploadBuffer<AtmosphereConstants>> AtmosphereCB = nullptr;

    // Значение fence для проверки, что GPU закончил с этим кадром
    UINT64 Fence = 0;
};