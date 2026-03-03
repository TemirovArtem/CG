#pragma once

#include "TerrainTypes.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

struct MeshGeometry;

namespace Terrain
{
    // Меш террейна: одна универсальная патч-сетка (вершины с UV, занавесы по краям). Позиция в мире вычисляется в вершинном шейдере по карте высот.
    
    class TerrainMesh
    {
    public:
        TerrainMesh();
        ~TerrainMesh();
        
        // Build the grid mesh
        // device: D3D12 device for resource creation
        // cmdList: Command list for upload commands
        // gridSize: Number of vertices per side (e.g., 65 for 65x65 grid)
        void Build(ID3D12Device* device,
                   ID3D12GraphicsCommandList* cmdList,
                   int gridSize = GRID_SIZE);
        
        // Get the mesh geometry for rendering
        MeshGeometry* Geometry() const { return mGrid.get(); }
        
        // Get mesh properties
        UINT GetVertexCount() const { return mVertexCount; }
        UINT GetIndexCount() const { return mIndexCount; }
        int GetGridSize() const { return mGridSize; }
        
        // Curtain statistics
        size_t GetBaseVertexCount() const;
        size_t GetCurtainVertexCount() const;
        size_t GetCurtainIndexCount() const;
        
        // Get vertex/index buffer views
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const;
        D3D12_INDEX_BUFFER_VIEW IndexBufferView() const;
        
    private:
        // Generate grid vertices and indices
        void GenerateGrid(int gridSize);
        
    private:
        std::unique_ptr<MeshGeometry> mGrid;
        
        // Cached mesh data for CPU-side operations
        std::vector<TerrainVertex> mVertices;
        std::vector<uint32_t> mIndices;
        
        UINT mVertexCount = 0;
        UINT mIndexCount = 0;
        int mGridSize = GRID_SIZE;
        
        // GPU resources
        Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBufferGPU;
        Microsoft::WRL::ComPtr<ID3D12Resource> mIndexBufferGPU;
        Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBufferUploader;
        Microsoft::WRL::ComPtr<ID3D12Resource> mIndexBufferUploader;
    };
    
}
