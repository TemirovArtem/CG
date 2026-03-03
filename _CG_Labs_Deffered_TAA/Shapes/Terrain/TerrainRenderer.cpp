// Рендерер террейна: инициализация D3D12, компиляция шейдеров, корневая подпись, текстуры, отрисовка узлов.
#include "TerrainRenderer.h"
#include "../../../Common/d3dUtil.h"
#include "../../../Common/d3dx12.h"
#include "../../../Common/UploadBuffer.h"
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
    }
    
    // Инициализация рендерера: форматы буферов, входной layout (только UV), компиляция шейдеров, корневая подпись
    void TerrainRenderer::Initialize(ID3D12Device* device,
                                      ID3D12GraphicsCommandList* cmdList,
                                      DXGI_FORMAT albedoFormat,
                                      DXGI_FORMAT normalFormat,
                                      DXGI_FORMAT velocityFormat,
                                      DXGI_FORMAT depthFormat)
    {
        mBackBufferFormat = albedoFormat;  // Store albedo format (used for PSO)
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
        
        // NOTE: BuildPSO will be called separately from ShapesApp after G-Buffer is created
    }
    
    void TerrainRenderer::CompileShaders()
    {
        mVSBytecode = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "VS", "vs_5_1");
        mPSBytecode = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "PS", "ps_5_1");
    }
    
    // Построение Root Signature: слоты для CB террейна, Pass CB, карты высот, альбедо, нормалей, сэмплеры
    void TerrainRenderer::BuildRootSignature(ID3D12Device* device)
    {
        // СЛОТЫ:
        // 0: CBV (b0) — TerrainDrawCB
        // 1: CBV (b1) — PassCB
        // 2: SRV (t0) — карта высот
        // 3: SRV (t1) — альбедо
        // 4: SRV (t2) — нормали
        
        CD3DX12_ROOT_PARAMETER rootParams[5];
        
        // Константный буфер на каждый draw узла террейна
        rootParams[0].InitAsConstantBufferView(0); // b0 = TerrainDrawCB
        
        // Константный буфер прохода (вид, проекция, время и т.д.)
        rootParams[1].InitAsConstantBufferView(1); // b1 = PassCB
        
        // Таблица SRV для карты высот (одна текстура на draw)
        CD3DX12_DESCRIPTOR_RANGE heightmapRange;
        heightmapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
        rootParams[2].InitAsDescriptorTable(1, &heightmapRange, D3D12_SHADER_VISIBILITY_ALL);
        
        // Таблица SRV для текстуры альбедо (цвет/погода)
        CD3DX12_DESCRIPTOR_RANGE albedoRange;
        albedoRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1
        rootParams[3].InitAsDescriptorTable(1, &albedoRange, D3D12_SHADER_VISIBILITY_PIXEL);
        
        // Таблица SRV для карты нормалей
        CD3DX12_DESCRIPTOR_RANGE normalRange;
        normalRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2
        rootParams[4].InitAsDescriptorTable(1, &normalRange, D3D12_SHADER_VISIBILITY_PIXEL);

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
                                    DXGI_FORMAT rtvFormat2,  // Add velocity buffer format
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
        psoDesc.NumRenderTargets = 3;  // NEW: Changed from 2 to 3
        psoDesc.RTVFormats[0] = rtvFormat0;
        psoDesc.RTVFormats[1] = rtvFormat1;
        psoDesc.RTVFormats[2] = rtvFormat2;  // NEW: Add velocity buffer format
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
        // Всего SRV: по 3 на тайл (heightmap, albedo, normal), 21*3 = 63
        int totalTextures = static_cast<int>(mHeightmaps.size());
        UINT totalSrvs = totalTextures * 3;
        
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
        
        // Дескрипторы SRV для карт высот
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
        
        // Дескрипторы SRV для карт альбедо
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
        
        // Дескрипторы SRV для карт нормалей
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
        
        // Порядок в куче: height (0-20), albedo (21-41), normals (42-62)
        ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        
        cmdList->SetGraphicsRootConstantBufferView(1, passCBAddress); // slot 1
        
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
            CD3DX12_GPU_DESCRIPTOR_HANDLE heightmapHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
            heightmapHandle.Offset(heightmapIdx, mSrvDescriptorSize);
            cmdList->SetGraphicsRootDescriptorTable(2, heightmapHandle); // slot 2
            CD3DX12_GPU_DESCRIPTOR_HANDLE albedoHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
            albedoHandle.Offset(totalTextures + heightmapIdx, mSrvDescriptorSize);
            cmdList->SetGraphicsRootDescriptorTable(3, albedoHandle); // slot 3
            CD3DX12_GPU_DESCRIPTOR_HANDLE normalHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
            normalHandle.Offset(2*totalTextures + heightmapIdx, mSrvDescriptorSize);
            cmdList->SetGraphicsRootDescriptorTable(4, normalHandle); // slot 4

            // Один DrawIndexedInstanced на тайл
            // Общий меш, но с разными CB и текстурами для каждого узла
            // Вершинный шейдер вычисляет позицию по UV и карте высот
            cmdList->DrawIndexedInstanced(mMesh->GetIndexCount(), 1, 0, 0, 0);
            
            // Отладочный вывод (можно отключить в релизе)
            std::cout << "[TERRAIN] Drawing node LOD=" << node->LOD << " at (" 
                     << node->MinXZ.x << "," << node->MinXZ.y << ") with seams\n";
        }
    }
    
} // namespace Terrain
