#include "TerrainMesh.h"
#include "../../../Common/d3dUtil.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace Terrain
{
    TerrainMesh::TerrainMesh()
        : mVertexCount(0)
        , mIndexCount(0)
        , mGridSize(GRID_SIZE)
    {
    }
    
    TerrainMesh::~TerrainMesh()
    {
    }
    
    void TerrainMesh::Build(ID3D12Device* device,
                            ID3D12GraphicsCommandList* cmdList,
                            int gridSize)
    {
        mGridSize = gridSize;
        
        // Генерим грид 65x65
        GenerateGrid(gridSize);
        
        mVertexCount = static_cast<UINT>(mVertices.size());
        mIndexCount = static_cast<UINT>(mIndices.size());
        
        const UINT vbByteSize = mVertexCount * sizeof(TerrainVertex);
        const UINT ibByteSize = mIndexCount * sizeof(uint32_t);
        
        // Создание объекта геометрии
        mGrid = std::make_unique<MeshGeometry>();
        mGrid->Name = "terrainGrid";
        
        // CPU буферы
        ThrowIfFailed(D3DCreateBlob(vbByteSize, &mGrid->VertexBufferCPU));
        CopyMemory(mGrid->VertexBufferCPU->GetBufferPointer(), mVertices.data(), vbByteSize);
        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mGrid->IndexBufferCPU));
        CopyMemory(mGrid->IndexBufferCPU->GetBufferPointer(), mIndices.data(), ibByteSize);
        
        // GPU буферы
        mGrid->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList,
            mVertices.data(), vbByteSize, mGrid->VertexBufferUploader);
        mGrid->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList,
            mIndices.data(), ibByteSize, mGrid->IndexBufferUploader);
        
        // Настройка свойств буферов
        mGrid->VertexByteStride = sizeof(TerrainVertex);
        mGrid->VertexBufferByteSize = vbByteSize;
        mGrid->IndexFormat = DXGI_FORMAT_R32_UINT;
        mGrid->IndexBufferByteSize = ibByteSize;
        
        // Подмеш
        SubmeshGeometry submesh;
        submesh.IndexCount = mIndexCount;
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;
        submesh.Bounds.Center = XMFLOAT3(0.5f, 0.0f, 0.5f);
        submesh.Bounds.Extents = XMFLOAT3(0.5f, 0.0f, 0.5f);
        
        mGrid->DrawArgs["grid"] = submesh;
        
        // В глобал указатели
        mVertexBufferGPU = mGrid->VertexBufferGPU;
        mIndexBufferGPU = mGrid->IndexBufferGPU;
    }
    
    // Генерация вершин (UV) и индексов: базовая сетка + четыре шторы по краям
    void TerrainMesh::GenerateGrid(int gridSize)
    {
        mVertices.clear();
        mIndices.clear();
        
        // Резерв: базовая сетка gridSize x gridSize + по 4 ребра с gridSize вершинами на каждое
        int vertexCount = gridSize * gridSize;
        int curtainVertexCount = gridSize * 4;
        int totalVertices = vertexCount + curtainVertexCount;
        
        int quadCount = (gridSize - 1) * (gridSize - 1);
        int curtainQuadCount = (gridSize - 1) * 4;
        int totalQuads = quadCount + curtainQuadCount;
        int indexCount = totalQuads * 6;  // 2 треугольника на квад, 3 индекса на треугольник
        
        mVertices.reserve(totalVertices);
        mIndices.reserve(indexCount);
        
        // Вершины базовой сетки: UV от [0,0] до [1,1]
        float step = 1.0f / static_cast<float>(gridSize - 1);
        
        for (int z = 0; z < gridSize; ++z)
        {
            for (int x = 0; x < gridSize; ++x)
            {
                TerrainVertex v;
                v.UV.x = x * step;
                v.UV.y = z * step;
                mVertices.push_back(v);
            }
        }
        
        // Вершины штор: дублируют рёбра сетки со смещением (в шейдере опускаются вниз)
        int baseVertexCount = vertexCount;
        
        // Штора по левому ребру (x=0)
        for (int z = 0; z < gridSize; ++z)
        {
            TerrainVertex v;
            v.UV.x = 0.0f;  // Fixed at left edge
            v.UV.y = z * step;
            mVertices.push_back(v);
        }
        
        // Штора по правому ребру (x=1)
        for (int z = 0; z < gridSize; ++z)
        {
            TerrainVertex v;
            v.UV.x = 1.0f;  // Fixed at right edge
            v.UV.y = z * step;
            mVertices.push_back(v);
        }
        
        // Штора по нижнему ребру (z=1 в UV, «низ» сетки)
        for (int x = 0; x < gridSize; ++x)
        {
            TerrainVertex v;
            v.UV.x = x * step;
            v.UV.y = 1.0f;  // Fixed at bottom edge (V=1 in texture coordinates)
            mVertices.push_back(v);
        }
        
        // Штора по верхнему ребру (z=0 в UV, «верх» сетки)
        for (int x = 0; x < gridSize; ++x)
        {
            TerrainVertex v;
            v.UV.x = x * step;
            v.UV.y = 0.0f;  // Fixed at top edge (V=0 in texture coordinates)
            mVertices.push_back(v);
        }
        
        // Индексы базовой сетки (два треугольника на каждый квад)
        for (int z = 0; z < gridSize - 1; ++z)
        {
            for (int x = 0; x < gridSize - 1; ++x)
            {
                uint32_t topLeft = z * gridSize + x;
                uint32_t topRight = topLeft + 1;
                uint32_t bottomLeft = (z + 1) * gridSize + x;
                uint32_t bottomRight = bottomLeft + 1;
                
                mIndices.push_back(topLeft);
                mIndices.push_back(bottomLeft);
                mIndices.push_back(topRight);
                
                mIndices.push_back(topRight);
                mIndices.push_back(bottomLeft);
                mIndices.push_back(bottomRight);
            }
        }
        
        // Индексы штор: квады между ребром сетки и вершинами штор
        int curtainStart = baseVertexCount;
        
        // Левая штора (соединяем левое ребро сетки с вершинами штор)
        for (int z = 0; z < gridSize - 1; ++z)
        {
            uint32_t gridTop = z * gridSize;           // Grid vertex at (0,z)
            uint32_t gridBottom = (z + 1) * gridSize;   // Grid vertex at (0,z+1)
            uint32_t curtainTop = curtainStart + z;     // Curtain vertex at (0,z)
            uint32_t curtainBottom = curtainStart + z + 1;
            
            mIndices.push_back(gridTop);
            mIndices.push_back(curtainTop);
            mIndices.push_back(gridBottom);
            
            mIndices.push_back(gridBottom);
            mIndices.push_back(curtainTop);
            mIndices.push_back(curtainBottom);
        }
        
        // Правая штора
        int rightStart = curtainStart + gridSize;
        for (int z = 0; z < gridSize - 1; ++z)
        {
            uint32_t gridTop = z * gridSize + (gridSize - 1);     // Grid vertex at (gridSize-1,z)
            uint32_t gridBottom = (z + 1) * gridSize + (gridSize - 1); // Grid vertex at (gridSize-1,z+1)
            uint32_t curtainTop = rightStart + z;      // Curtain vertex at (1,z)
            uint32_t curtainBottom = rightStart + z + 1;
            
            mIndices.push_back(gridTop);
            mIndices.push_back(gridBottom);
            mIndices.push_back(curtainTop);
            
            mIndices.push_back(gridBottom);
            mIndices.push_back(curtainBottom);
            mIndices.push_back(curtainTop);
        }
        
        // Нижняя штора
        int bottomStart = rightStart + gridSize;
        for (int x = 0; x < gridSize - 1; ++x)
        {
            uint32_t gridLeft = (gridSize - 1) * gridSize + x;     // Grid vertex at (x,gridSize-1)
            uint32_t gridRight = (gridSize - 1) * gridSize + x + 1; // Grid vertex at (x+1,gridSize-1)
            uint32_t curtainLeft = bottomStart + x;    // Curtain vertex at (x,1)
            uint32_t curtainRight = bottomStart + x + 1;
            
            mIndices.push_back(gridLeft);
            mIndices.push_back(curtainLeft);
            mIndices.push_back(gridRight);
            
            mIndices.push_back(gridRight);
            mIndices.push_back(curtainLeft);
            mIndices.push_back(curtainRight);
        }
        
        // Верхняя штора
        int topStart = bottomStart + gridSize;
        for (int x = 0; x < gridSize - 1; ++x)
        {
            uint32_t gridLeft = x;                     // Grid vertex at (x,0)
            uint32_t gridRight = x + 1;                // Grid vertex at (x+1,0)
            uint32_t curtainLeft = topStart + x;       // Curtain vertex at (x,0)
            uint32_t curtainRight = topStart + x + 1;
            
            mIndices.push_back(gridLeft);
            mIndices.push_back(gridRight);
            mIndices.push_back(curtainLeft);
            
            mIndices.push_back(gridRight);
            mIndices.push_back(curtainRight);
            mIndices.push_back(curtainLeft);
        }
    }
    
    // Возвращает представление вершинного буфера для IASetVertexBuffers
    D3D12_VERTEX_BUFFER_VIEW TerrainMesh::VertexBufferView() const
    {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = mVertexBufferGPU->GetGPUVirtualAddress();
        vbv.StrideInBytes = sizeof(TerrainVertex);
        vbv.SizeInBytes = mVertexCount * sizeof(TerrainVertex);
        return vbv;
    }
    
    // Возвращает представление индексного буфера для IASetIndexBuffer
    D3D12_INDEX_BUFFER_VIEW TerrainMesh::IndexBufferView() const
    {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = mIndexBufferGPU->GetGPUVirtualAddress();
        ibv.Format = DXGI_FORMAT_R32_UINT;
        ibv.SizeInBytes = mIndexCount * sizeof(uint32_t);
        return ibv;
    }
    
    // Количество вершин базовой сетки (без штор)
    size_t TerrainMesh::GetBaseVertexCount() const
    {
        int gridSize = GetGridSize();
        return static_cast<size_t>(gridSize * gridSize);
    }
    
    // Количество вершин штор (4 ребра по gridSize вершин)
    size_t TerrainMesh::GetCurtainVertexCount() const
    {
        int gridSize = GetGridSize();
        return static_cast<size_t>(gridSize * 4);
    }
    
    // Количество индексов занавесов: 4 ребра × (gridSize-1) квадов × 6 индексов
    size_t TerrainMesh::GetCurtainIndexCount() const
    {
        int gridSize = GetGridSize();
        return static_cast<size_t>((gridSize - 1) * 4 * 6);
    }
    
}