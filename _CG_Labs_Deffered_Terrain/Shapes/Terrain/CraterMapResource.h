#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace Terrain
{
    // Encapsulates the crater deformation texture (R32_FLOAT format) with UAV and SRV access
    // The CraterMap accumulates terrain deformations and is combined with the HeightMap in shaders
    class CraterMapResource
    {
    public:
        CraterMapResource();
        ~CraterMapResource();
        
        // Initialize the crater map resource with specified dimensions
        // device: D3D12 device for resource creation
        // width: Texture width (should match HeightMap width)
        // height: Texture height (should match HeightMap height)
        // srvHeap: Descriptor heap for SRV allocation
        // uavHeap: Descriptor heap for UAV allocation
        // srvOffset: Offset in SRV heap for this resource's SRV
        // uavOffset: Offset in UAV heap for this resource's UAV
        void Initialize(ID3D12Device* device, 
                        UINT width, 
                        UINT height,
                        ID3D12DescriptorHeap* srvHeap,
                        ID3D12DescriptorHeap* uavHeap,
                        UINT srvOffset,
                        UINT uavOffset);
        
        // Clear the crater map to zero (initial state)
        // cmdList: Command list for clear operation
        void Clear(ID3D12GraphicsCommandList* cmdList);
        
        // Accessors
        ID3D12Resource* GetResource() const { return mCraterMap.Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetSRV() const { return mSRV; }
        D3D12_CPU_DESCRIPTOR_HANDLE GetUAV() const { return mUAV; }
        D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGpu() const { return mSRVGpu; }
        D3D12_GPU_DESCRIPTOR_HANDLE GetUAVGpu() const { return mUAVGpu; }
        
        UINT GetWidth() const { return mWidth; }
        UINT GetHeight() const { return mHeight; }
        
    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> mCraterMap;
        D3D12_CPU_DESCRIPTOR_HANDLE mSRV;
        D3D12_CPU_DESCRIPTOR_HANDLE mUAV;
        D3D12_GPU_DESCRIPTOR_HANDLE mSRVGpu;
        D3D12_GPU_DESCRIPTOR_HANDLE mUAVGpu;
        UINT mWidth = 0;
        UINT mHeight = 0;
    };
}
