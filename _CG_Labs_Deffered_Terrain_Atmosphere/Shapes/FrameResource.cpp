#include "FrameResource.h"

// Создание ресурсов кадра: аллокатор команд и upload-буферы для Pass, Material, Object и (опционально) Terrain CB
FrameResource::FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount, UINT terrainNodeCount)
{
    ThrowIfFailed(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

    PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, passCount, true);
    MaterialCB = std::make_unique<UploadBuffer<MaterialConstants>>(device, materialCount, true);
    ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);
    
    if (terrainNodeCount > 0)
    {
        TerrainCB = std::make_unique<UploadBuffer<Terrain::TerrainDrawCB>>(device, terrainNodeCount, true);
    }
    
    // Атмосфера: один CB на кадр
    AtmosphereCB = std::make_unique<UploadBuffer<AtmosphereConstants>>(device, 1, true);
}

FrameResource::~FrameResource()
{

}