#include "CraterMapResource.h"
#include "../../../Common/d3dUtil.h"
#include "../../../Common/d3dx12.h"

using namespace Terrain;
using Microsoft::WRL::ComPtr;

CraterMapResource::CraterMapResource()
{
}

CraterMapResource::~CraterMapResource()
{
}

void CraterMapResource::Initialize(ID3D12Device* device, 
                                    UINT width, 
                                    UINT height,
                                    ID3D12DescriptorHeap* srvHeap,
                                    ID3D12DescriptorHeap* uavHeap,
                                    UINT srvOffset,
                                    UINT uavOffset)
{
    mWidth = width;
    mHeight = height;
    
    // Create the CraterMap texture resource
    // Format: R32_FLOAT for precise accumulation of deformation values
    // Flags: ALLOW_UNORDERED_ACCESS for compute shader writes
    // Initial State: UNORDERED_ACCESS for compute shader access
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;
    
    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&mCraterMap)));
    
    // Create Shader Resource View (SRV) for domain shader reads
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    
    UINT srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mSRV = CD3DX12_CPU_DESCRIPTOR_HANDLE(srvHeap->GetCPUDescriptorHandleForHeapStart(), srvOffset, srvDescriptorSize);
    mSRVGpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(srvHeap->GetGPUDescriptorHandleForHeapStart(), srvOffset, srvDescriptorSize);
    device->CreateShaderResourceView(mCraterMap.Get(), &srvDesc, mSRV);
    
    // Create Unordered Access View (UAV) for compute shader writes
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;
    
    UINT uavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mUAV = CD3DX12_CPU_DESCRIPTOR_HANDLE(uavHeap->GetCPUDescriptorHandleForHeapStart(), uavOffset, uavDescriptorSize);
    mUAVGpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(uavHeap->GetGPUDescriptorHandleForHeapStart(), uavOffset, uavDescriptorSize);
    device->CreateUnorderedAccessView(mCraterMap.Get(), nullptr, &uavDesc, mUAV);
}

void CraterMapResource::Clear(ID3D12GraphicsCommandList* cmdList)
{
    // Clear the crater map to zero using ClearUnorderedAccessViewFloat
    // This initializes the deformation texture to have no deformations
    
    // Transition to UNORDERED_ACCESS if not already in that state
    // (The resource is created in UNORDERED_ACCESS state, so this is typically not needed on first clear)
    
    FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearUnorderedAccessViewFloat(mUAVGpu, mUAV, mCraterMap.Get(), clearColor, 0, nullptr);
}
