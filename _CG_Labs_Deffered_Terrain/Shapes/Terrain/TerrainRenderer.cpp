// Рендерер террейна: инициализация D3D12, компиляция шейдеров, корневая подпись, текстуры, отрисовка узлов.
#include "TerrainRenderer.h"
#include "../../../Common/d3dUtil.h"
#include "../../../Common/d3dx12.h"
#include "../../../Common/UploadBuffer.h"
#include "Common/DebugFlags.h"
#include <d3dcompiler.h>
#include <iostream>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace Terrain
{
    TerrainRenderer::TerrainRenderer()
    {
    }
    
    TerrainRenderer::~TerrainRenderer()
    {
        // Clean up crater deformation resources
        if (mCraterParamsCB)
        {
            delete mCraterParamsCB;
            mCraterParamsCB = nullptr;
        }
    }
    
    // Инициализация рендерера: форматы буферов, входной layout (только UV), компиляция шейдеров, корневая подпись
    void TerrainRenderer::Initialize(ID3D12Device* device,
                                      ID3D12GraphicsCommandList* cmdList,
                                      DXGI_FORMAT backBufferFormat,
                                      DXGI_FORMAT depthFormat)
    {
        mBackBufferFormat = backBufferFormat;
        mDepthFormat = depthFormat;
        
        mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        
        // InputLayout террейна: только текстурные координаты тк позицию посчитали в вершинном шейдере по heightmap
        mInputLayout = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        
        // Вершинный и пиксельный шейдеры
        CompileShaders();
        
        // RootSignature (привязки CBV/SRV к слотам)
        BuildRootSignature(device);
    }
    
    void TerrainRenderer::CompileShaders()
    {
        mVSBytecode = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "VS", "vs_5_1");
        mPSBytecode = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "PS", "ps_5_1");
    }
    
    // Построение Root Signature: слоты для CB террейна, Pass CB, карты высот, CraterMap, альбедо, нормалей, сэмплеры
    void TerrainRenderer::BuildRootSignature(ID3D12Device* device)
    {
        // СЛОТЫ:
        // 0: CBV (b0) — TerrainDrawCB
        // 1: CBV (b1) — PassCB
        // 2: SRV (t0) — карта высот
        // 3: SRV (t1) — CraterMap (NEW)
        // 4: SRV (t2) — альбедо (moved from t1)
        // 5: SRV (t3) — нормали (moved from t2)
        
        CD3DX12_ROOT_PARAMETER rootParams[6];
        
        // Константный буфер на каждый draw узла террейна
        rootParams[0].InitAsConstantBufferView(0); // b0 = TerrainDrawCB
        
        // Константный буфер прохода (вид, проекция, время и т.д.)
        rootParams[1].InitAsConstantBufferView(1); // b1 = PassCB
        
        // Таблица SRV для карты высот (одна текстура на draw)
        CD3DX12_DESCRIPTOR_RANGE heightmapRange;
        heightmapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
        rootParams[2].InitAsDescriptorTable(1, &heightmapRange, D3D12_SHADER_VISIBILITY_ALL);
        
        // Таблица SRV для CraterMap (NEW)
        CD3DX12_DESCRIPTOR_RANGE craterMapRange;
        craterMapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1
        rootParams[3].InitAsDescriptorTable(1, &craterMapRange, D3D12_SHADER_VISIBILITY_ALL);
        
        // Таблица SRV для текстуры альбедо (цвет/погода) - moved from t1 to t2
        CD3DX12_DESCRIPTOR_RANGE albedoRange;
        albedoRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2
        rootParams[4].InitAsDescriptorTable(1, &albedoRange, D3D12_SHADER_VISIBILITY_PIXEL);
        
        // Таблица SRV для карты нормалей - moved from t2 to t3
        CD3DX12_DESCRIPTOR_RANGE normalRange;
        normalRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3); // t3
        rootParams[5].InitAsDescriptorTable(1, &normalRange, D3D12_SHADER_VISIBILITY_PIXEL);

        // Статические сэмплеры (линейная фильтрация, wrap и clamp)
        CD3DX12_STATIC_SAMPLER_DESC linearWrap(
            0, // shaderRegister
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        
        CD3DX12_STATIC_SAMPLER_DESC linearClamp(
            1, // shaderRegister
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        
        std::array<CD3DX12_STATIC_SAMPLER_DESC, 2> samplers = { linearWrap, linearClamp };
        
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(rootParams), rootParams,
            static_cast<UINT>(samplers.size()), samplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        
        ComPtr<ID3DBlob> serializedRootSig;
        ComPtr<ID3DBlob> errorBlob;
        
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
        
        if (errorBlob)
        {
            OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
        
        ThrowIfFailed(device->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(mRootSignature.GetAddressOf())));
    }
    
    // PSO
    void TerrainRenderer::BuildPSO(ID3D12Device* device,
                                    DXGI_FORMAT rtvFormat0,
                                    DXGI_FORMAT rtvFormat1,
                                    DXGI_FORMAT dsvFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { mInputLayout.data(), static_cast<UINT>(mInputLayout.size()) };
        psoDesc.pRootSignature = mRootSignature.Get();
        psoDesc.VS = {
            reinterpret_cast<BYTE*>(mVSBytecode->GetBufferPointer()),
            mVSBytecode->GetBufferSize()
        };
        psoDesc.PS = {
            reinterpret_cast<BYTE*>(mPSBytecode->GetBufferPointer()),
            mPSBytecode->GetBufferSize()
        };
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 2;
        psoDesc.RTVFormats[0] = rtvFormat0;
        psoDesc.RTVFormats[1] = rtvFormat1;
        psoDesc.DSVFormat = dsvFormat;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
        
        ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
    }
    
    // Загрузка текстур всех лодов
    void TerrainRenderer::LoadTextures(ID3D12Device* device,
                                        ID3D12GraphicsCommandList* cmdList,
                                        const std::wstring& basePath)
    {
        // Всего 21 текстура каждого типа(albedo, normal, deapth): 1 + 4 + 16 (LOD0, LOD1, LOD2)
        int totalTextures = 1 + 4 + 16;
        mHeightmaps.resize(totalTextures);
        mHeightmapUploaders.resize(totalTextures);
        mAlbedoMaps.resize(totalTextures);
        mAlbedoUploaders.resize(totalTextures);
        mNormalMaps.resize(totalTextures);
        mNormalUploaders.resize(totalTextures);
        
        // Загрузка текстур LOD0 (один тайл)
        {
            std::wstring heightPath = basePath + L"lod0/Height/Height.dds";
            std::wstring albedoPath = basePath + L"lod0/Weathering/Weathering.dds";
            std::wstring normalPath = basePath + L"lod0/Normals/Normals.dds";
            
            LoadTexture(device, cmdList, heightPath, mHeightmaps[0], mHeightmapUploaders[0]);
            LoadTexture(device, cmdList, albedoPath, mAlbedoMaps[0], mAlbedoUploaders[0]);
            LoadTexture(device, cmdList, normalPath, mNormalMaps[0], mNormalUploaders[0]);
        }
        
        // Загрузка текстур LOD1 (сетка 2x2, 4 тайла)
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                int idx = HEIGHTMAP_LOD1_START + y * 2 + x;
                
                std::wstring suffix = L"_y" + std::to_wstring(y) + L"_x" + std::to_wstring(x) + L".dds";
                std::wstring heightPath = basePath + L"lod1/Height/Height" + suffix;
                std::wstring albedoPath = basePath + L"lod1/Weathering/Weathering" + suffix;
                std::wstring normalPath = basePath + L"lod1/Normals/Normals" + suffix;
                
                LoadTexture(device, cmdList, heightPath, mHeightmaps[idx], mHeightmapUploaders[idx]);
                LoadTexture(device, cmdList, albedoPath, mAlbedoMaps[idx], mAlbedoUploaders[idx]);
                LoadTexture(device, cmdList, normalPath, mNormalMaps[idx], mNormalUploaders[idx]);
            }
        }
        
        // Загрузка текстур LOD2 (сетка 4x4, 16 тайлов)
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                int idx = HEIGHTMAP_LOD2_START + y * 4 + x;
                
                std::wstring suffix = L"_y" + std::to_wstring(y) + L"_x" + std::to_wstring(x) + L".dds";
                std::wstring heightPath = basePath + L"lod2/Height/Height" + suffix;
                std::wstring albedoPath = basePath + L"lod2/Weathering/Weathering" + suffix;
                std::wstring normalPath = basePath + L"lod2/Normals/Normals" + suffix;
                
                LoadTexture(device, cmdList, heightPath, mHeightmaps[idx], mHeightmapUploaders[idx]);
                LoadTexture(device, cmdList, albedoPath, mAlbedoMaps[idx], mAlbedoUploaders[idx]);
                LoadTexture(device, cmdList, normalPath, mNormalMaps[idx], mNormalUploaders[idx]);
            }
        }
    }
    
    // Загрузка одной текстуры
    void TerrainRenderer::LoadTexture(ID3D12Device* device,
                                       ID3D12GraphicsCommandList* cmdList,
                                       const std::wstring& path,
                                       ComPtr<ID3D12Resource>& resource,
                                       ComPtr<ID3D12Resource>& uploadHeap)
    {
        HRESULT hr = DirectX::CreateDDSTextureFromFile12(device, cmdList, path.c_str(), resource, uploadHeap);
    }
    
    // Построение кучи дескрипторов SRV
    void TerrainRenderer::BuildSrvHeap(ID3D12Device* device)
    {
        // SRV layout:
        // Offset 0-20:  HeightMaps (LOD0: 1, LOD1: 4, LOD2: 16) = 21 textures
        // Offset 21:    CraterMap SRV (NEW)
        // Offset 22-42: AlbedoMaps (21 textures)
        // Offset 43-63: NormalMaps (21 textures)
        // Total: 64 SRVs
        
        int totalTextures = static_cast<int>(mHeightmaps.size()); // 21
        UINT totalSrvs = totalTextures * 3 + 1; // 21*3 + 1 (CraterMap) = 64
        
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = totalSrvs;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        
        ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap)));
        
        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(mSrvHeap->GetCPUDescriptorHandleForHeapStart());
        

        
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        
        // Дескрипторы SRV для карт высот (offset 0-20)
        for (int i = 0; i < totalTextures; ++i)
        {
            if (mHeightmaps[i])
            {
                auto desc = mHeightmaps[i]->GetDesc();
                srvDesc.Format = desc.Format;
                srvDesc.Texture2D.MipLevels = desc.MipLevels;
                device->CreateShaderResourceView(mHeightmaps[i].Get(), &srvDesc, cpuHandle);
            }
            cpuHandle.Offset(1, mSrvDescriptorSize);
        }
        
        // Дескриптор SRV для CraterMap (offset 21) - NEW
        if (mCraterMap)
        {
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(mCraterMap.Get(), &srvDesc, cpuHandle);
        }
        cpuHandle.Offset(1, mSrvDescriptorSize);
        
        // Дескрипторы SRV для карт альбедо (offset 22-42)
        for (int i = 0; i < totalTextures; ++i)
        {
            if (mAlbedoMaps[i])
            {
                auto desc = mAlbedoMaps[i]->GetDesc();
                srvDesc.Format = desc.Format;
                srvDesc.Texture2D.MipLevels = desc.MipLevels;
                device->CreateShaderResourceView(mAlbedoMaps[i].Get(), &srvDesc, cpuHandle);
            }
            cpuHandle.Offset(1, mSrvDescriptorSize);
        }
        
        // Дескрипторы SRV для карт нормалей (offset 43-63)
        for (int i = 0; i < totalTextures; ++i)
        {
            if (mNormalMaps[i])
            {
                auto desc = mNormalMaps[i]->GetDesc();
                srvDesc.Format = desc.Format;
                srvDesc.Texture2D.MipLevels = desc.MipLevels;
                device->CreateShaderResourceView(mNormalMaps[i].Get(), &srvDesc, cpuHandle);
            }
            cpuHandle.Offset(1, mSrvDescriptorSize);
        }
    }
    

    void TerrainRenderer::Draw(ID3D12GraphicsCommandList* cmdList,
                                 const std::vector<TerrainNode*>& nodes,
                                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddress,
                                 UploadBuffer<TerrainDrawCB>* terrainCB)
    {
        if (!mMesh || nodes.empty()) return;
        
        cmdList->SetPipelineState(mPSO.Get());
        cmdList->SetGraphicsRootSignature(mRootSignature.Get());
        
        // SRV heap layout:
        // Offset 0-20:  HeightMaps
        // Offset 21:    CraterMap (NEW)
        // Offset 22-42: AlbedoMaps
        // Offset 43-63: NormalMaps
        ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        
        cmdList->SetGraphicsRootConstantBufferView(1, passCBAddress); // slot 1
        
        // Bind CraterMap SRV (offset 21) to slot 3 (t1) - NEW
        CD3DX12_GPU_DESCRIPTOR_HANDLE craterMapHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
        craterMapHandle.Offset(21, mSrvDescriptorSize); // CraterMap at offset 21
        cmdList->SetGraphicsRootDescriptorTable(3, craterMapHandle); // slot 3 = t1
        
        D3D12_VERTEX_BUFFER_VIEW vbv = mMesh->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibv = mMesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        
        int totalTextures = static_cast<int>(mHeightmaps.size());  // 21 текстура каждого типа
        
        // Рисуем каждый тайл
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const TerrainNode* node = nodes[i];
            
            TerrainDrawCB drawCB;
            
            // Позиция и размер тайла в мировых координатах (XZ плоскость)
            drawCB.NodeMinXZ = node->MinXZ;  // Левый нижний угол тайла
            drawCB.NodeSize = node->Size;    // Длина стороны тайла
            
            drawCB.LOD = node->LOD;
            
            // Масштаб высоты (преобразует нормализованную высоту [0..1] в мировые единицы)
            drawCB.HeightScale = TERRAIN_HEIGHT_SCALE;
            
            // Индекс heightmap в массиве текстур (0-20)
            drawCB.HeightmapIndex = node->HeightmapIndex;
            
            // Параметры штор
            drawCB.CurtainHeight = GetCurtainHeight();  // На сколько опускаются занавесы
            drawCB.CurtainExtension = 1.0f;  // Горизонтальное расширение занавесов
            
            // Битмаска рёбер с шторами: 0=лево, 1=право, 2=верх, 3=низ
            // 0xF = 1111 в двоичном = все четыре ребра
            drawCB.CurtainEdges = 0xF;
            
            // UV-область тайла в карте высот
            // Каждый тайл покрывает [0,1]×[0,1] в своей текстуре
            drawCB.HeightmapMinUV = XMFLOAT2(0.0f, 0.0f);
            drawCB.HeightmapSizeUV = XMFLOAT2(1.0f, 1.0f);
            
            // Копируем данные CB в upload-буфер (CPU → GPU)
            terrainCB->CopyData(static_cast<int>(i), drawCB);
            
            // GPU-адрес этого CB в upload-буфере
            D3D12_GPU_VIRTUAL_ADDRESS cbAddress = terrainCB->Resource()->GetGPUVirtualAddress() +
                i * d3dUtil::CalcConstantBufferByteSize(sizeof(TerrainDrawCB));
            
            // Terrain CB к слоту 0
            cmdList->SetGraphicsRootConstantBufferView(0, cbAddress); // slot 0
            
            // Привязка SRV текстур для этого тайла
            int heightmapIdx = node->HeightmapIndex;
            
            // HeightMap at offset 0-20, bind to slot 2 (t0)
            CD3DX12_GPU_DESCRIPTOR_HANDLE heightmapHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
            heightmapHandle.Offset(heightmapIdx, mSrvDescriptorSize);
            cmdList->SetGraphicsRootDescriptorTable(2, heightmapHandle); // slot 2 = t0
            
            // AlbedoMap at offset 22-42, bind to slot 4 (t2) - shifted from slot 3
            CD3DX12_GPU_DESCRIPTOR_HANDLE albedoHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
            albedoHandle.Offset(22 + heightmapIdx, mSrvDescriptorSize); // offset 22 + index
            cmdList->SetGraphicsRootDescriptorTable(4, albedoHandle); // slot 4 = t2
            
            // NormalMap at offset 43-63, bind to slot 5 (t3) - shifted from slot 4
            CD3DX12_GPU_DESCRIPTOR_HANDLE normalHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
            normalHandle.Offset(43 + heightmapIdx, mSrvDescriptorSize); // offset 43 + index
            cmdList->SetGraphicsRootDescriptorTable(5, normalHandle); // slot 5 = t3

            // Один DrawIndexedInstanced на тайл
            // Общий меш, но с разными CB и текстурами для каждого узла
            // Вершинный шейдер вычисляет позицию по UV и карте высот
            cmdList->DrawIndexedInstanced(mMesh->GetIndexCount(), 1, 0, 0, 0);
            
            // Отладочный вывод (можно отключить в релизе)
            if (DebugFlags::TerrainRendering)
            {
                std::cout << "[TERRAIN] Drawing node LOD=" << node->LOD << " at (" 
                         << node->MinXZ.x << "," << node->MinXZ.y << ") with seams\n";
            }
        }
    }
    
    // Crater deformation implementation
    
    void TerrainRenderer::CompileComputeShader()
    {
        mComputeShader = d3dUtil::CompileShader(L"Shaders\\CraterDeformation.hlsl", nullptr, "CS", "cs_5_1");
    }
    
    void TerrainRenderer::BuildComputeRootSignature(ID3D12Device* device)
    {
        // Root signature for compute shader:
        // 0: CBV (b0) - CraterParams constant buffer
        // 1: Descriptor Table - UAV (u0) - CraterMap
        
        CD3DX12_ROOT_PARAMETER rootParams[2];
        
        // Constant buffer for crater parameters
        rootParams[0].InitAsConstantBufferView(0); // b0 = CraterParams
        
        // UAV descriptor table for CraterMap
        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0
        rootParams[1].InitAsDescriptorTable(1, &uavRange);
        
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(rootParams), rootParams,
            0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);
        
        ComPtr<ID3DBlob> serializedRootSig;
        ComPtr<ID3DBlob> errorBlob;
        
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
        
        if (errorBlob)
        {
            OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
        
        ThrowIfFailed(device->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(mComputeRootSig.GetAddressOf())));
    }
    
    void TerrainRenderer::BuildComputePSO(ID3D12Device* device)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mComputeRootSig.Get();
        psoDesc.CS = {
            reinterpret_cast<BYTE*>(mComputeShader->GetBufferPointer()),
            mComputeShader->GetBufferSize()
        };
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        
        ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mComputePSO)));
    }
    
    void TerrainRenderer::InitializeCraterMap(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height)
    {
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Initializing CraterMap: " << width << "x" << height << std::endl;
        
        mCraterMapWidth = width;
        mCraterMapHeight = height;
        
        // 1. Create CraterMap resource in UAV state
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&mCraterMap)));
        
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] CraterMap resource created" << std::endl;
        
        // 2. Create UAV descriptor heap
        D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
        uavHeapDesc.NumDescriptors = 1;
        uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        
        ThrowIfFailed(device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&mUavHeap)));
        
        // 3. Create UAV
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        
        device->CreateUnorderedAccessView(mCraterMap.Get(), nullptr, &uavDesc, 
            mUavHeap->GetCPUDescriptorHandleForHeapStart());
        
        // 4. Create constant buffer for crater parameters
        mCraterParamsCB = new UploadBuffer<CraterParams>(device, 1, true);
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Created CraterParams constant buffer" << std::endl;
        
        // 5. Compile compute shader and build compute pipeline
        CompileComputeShader();
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Compiled compute shader" << std::endl;
        
        BuildComputeRootSignature(device);
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Built compute root signature" << std::endl;
        
        BuildComputePSO(device);
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Built compute PSO" << std::endl;
        
        // 6. Clear CraterMap to zero using compute shader
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Clearing CraterMap to zero using compute shader..." << std::endl;
        
        // Dispatch a clear operation (set center far outside, radius 0, depth 0)
        // This will effectively do nothing, but we'll use a simple clear shader instead
        // For now, we'll just leave it uninitialized and rely on the first frame to clear it
        // OR we can dispatch with a huge radius to cover entire texture
        CraterParams clearParams;
        clearParams.centerUV = DirectX::XMFLOAT2(0.5f, 0.5f);
        clearParams.radiusUV = 2.0f; // Covers entire texture
        clearParams.depth = 0.0f;    // Set to zero (clear operation)
        
        mCraterParamsCB->CopyData(0, clearParams);
        
        cmdList->SetPipelineState(mComputePSO.Get());
        cmdList->SetComputeRootSignature(mComputeRootSig.Get());
        cmdList->SetComputeRootConstantBufferView(0, mCraterParamsCB->Resource()->GetGPUVirtualAddress());
        
        ID3D12DescriptorHeap* heaps[] = { mUavHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        cmdList->SetComputeRootDescriptorTable(1, mUavHeap->GetGPUDescriptorHandleForHeapStart());
        
        UINT groupCountX = (mCraterMapWidth + 7) / 8;
        UINT groupCountY = (mCraterMapHeight + 7) / 8;
        cmdList->Dispatch(groupCountX, groupCountY, 1);
        
        // Insert UAV barrier
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = mCraterMap.Get();
        cmdList->ResourceBarrier(1, &barrier);
        
        if (DebugFlags::TerrainCraterMap)
        {
            std::cout << "[TERRAIN RENDERER] CraterMap cleared to zero" << std::endl;
            std::cout << "[TERRAIN RENDERER] CraterMap initialization complete!" << std::endl;
        }
    }
    
    void TerrainRenderer::DispatchCraterDeformation(
        ID3D12GraphicsCommandList* cmdList,
        const DirectX::XMFLOAT2& centerUV,
        float radiusUV,
        float depth)
    {
        if (DebugFlags::TerrainCraterMap)
        {
            std::cout << "[TERRAIN RENDERER] DispatchCraterDeformation called: UV(" 
                      << centerUV.x << ", " << centerUV.y << "), radius=" 
                      << radiusUV << ", depth=" << depth << std::endl;
        }
        
        // Check if resources are initialized
        if (!mCraterParamsCB)
        {
            if (DebugFlags::TerrainCraterMap)
                std::cout << "[TERRAIN RENDERER] ERROR: mCraterParamsCB is nullptr!" << std::endl;
            return;
        }
        
        if (!mCraterMap)
        {
            if (DebugFlags::TerrainCraterMap)
                std::cout << "[TERRAIN RENDERER] ERROR: mCraterMap is nullptr!" << std::endl;
            return;
        }
        
        if (!mComputePSO)
        {
            if (DebugFlags::TerrainCraterMap)
                std::cout << "[TERRAIN RENDERER] ERROR: mComputePSO is nullptr!" << std::endl;
            return;
        }
        // 0. Transition CraterMap back to UNORDERED_ACCESS if needed
        // On first frame, it's already in UNORDERED_ACCESS (initial state)
        // On subsequent frames, it's in PIXEL_SHADER_RESOURCE after rendering
        if (mCraterMapNeedsTransitionToUAV)
        {
            D3D12_RESOURCE_BARRIER toUAV = {};
            toUAV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toUAV.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toUAV.Transition.pResource = mCraterMap.Get();
            toUAV.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toUAV.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toUAV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &toUAV);
        }
        
        // 1. Update constant buffer
        CraterParams params;
        params.centerUV = centerUV;
        params.radiusUV = radiusUV;
        params.depth = depth;
        mCraterParamsCB->CopyData(0, params);
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN RENDERER] Updated constant buffer" << std::endl;
        
        // 2. Set compute pipeline state
        cmdList->SetPipelineState(mComputePSO.Get());
        cmdList->SetComputeRootSignature(mComputeRootSig.Get());
        
        // 3. Bind resources
        cmdList->SetComputeRootConstantBufferView(0, mCraterParamsCB->Resource()->GetGPUVirtualAddress());
        
        ID3D12DescriptorHeap* heaps[] = { mUavHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        cmdList->SetComputeRootDescriptorTable(1, mUavHeap->GetGPUDescriptorHandleForHeapStart());
        
        // 4. Dispatch compute shader
        UINT groupCountX = (mCraterMapWidth + 7) / 8;
        UINT groupCountY = (mCraterMapHeight + 7) / 8;
        cmdList->Dispatch(groupCountX, groupCountY, 1);
        if (DebugFlags::TerrainCraterMap)
        {
            std::cout << "[TERRAIN RENDERER] Dispatched compute shader: " 
                      << groupCountX << "x" << groupCountY << " thread groups" << std::endl;
        }
        
        // 5. Insert UAV barrier
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = mCraterMap.Get();
        cmdList->ResourceBarrier(1, &barrier);
        
        // 6. Transition CraterMap from UNORDERED_ACCESS to PIXEL_SHADER_RESOURCE
        D3D12_RESOURCE_BARRIER transition = {};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        transition.Transition.pResource = mCraterMap.Get();
        transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &transition);
        
        // Mark that CraterMap needs transition back to UAV on next dispatch
        mCraterMapNeedsTransitionToUAV = true;
    }

} // namespace Terrain
