#pragma once

#include "TerrainTypes.h"
#include "TerrainQuadtree.h"
#include "TerrainMesh.h"
#include "TerrainRenderer.h"
#include <d3d12.h>
#include <memory>
#include <string>

class Camera;
template<typename T> class UploadBuffer;

namespace Terrain
{  
    class TerrainSystem
    {
    public:
        TerrainSystem();
        ~TerrainSystem();
        
        // Initialize the terrain system
        // device: D3D12 device
        // cmdList: Command list for initialization commands
        // textureBasePath: Path to terrain textures folder
        void Initialize(ID3D12Device* device,
                        ID3D12GraphicsCommandList* cmdList,
                        const std::wstring& textureBasePath = L"Textures/Terrain/");
        
        // Build GPU resources that depend on render target formats
        void BuildPSO(ID3D12Device* device,
                      DXGI_FORMAT albedoFormat,
                      DXGI_FORMAT normalFormat,
                      DXGI_FORMAT depthFormat);
        
        // Update terrain state (culling, LOD selection)
        // camera: Current camera for frustum culling and LOD selection
        void Update(const Camera& camera, bool mDebugCulling, float FOVFrustum);
        
        // Draw the terrain
        // cmdList: Command list for draw calls
        // passCBAddress: GPU address of the pass constant buffer
        // terrainCB: Upload buffer for per-draw terrain constants
        void Draw(ID3D12GraphicsCommandList* cmdList,
                  D3D12_GPU_VIRTUAL_ADDRESS passCBAddress,
                  UploadBuffer<TerrainDrawCB>* terrainCB);
        
        // Get visible node count (for UI/debug)
        int GetVisibleNodeCount() const;
        int GetTotalNodeCount() const;
        
        // Configuration
        void SetWorldSize(float size);
        float GetWorldSize() const { return mWorldSize; }
        
        void SetHeightScale(float scale) { mHeightScale = scale; }
        float GetHeightScale() const { return mHeightScale; }
        
        void SetLODDistances(float lod0, float lod1);
        float GetLOD0Distance() const;
        float GetLOD1Distance() const;
        
        // Access sub-components (for advanced use/debugging)
        TerrainQuadtree& GetQuadtree() { return mQuadtree; }
        TerrainMesh& GetMesh() { return mMesh; }
        TerrainRenderer& GetRenderer() { return mRenderer; }
        
        // Get max visible nodes (for CB allocation)
        static constexpr int GetMaxVisibleNodes() { return MAX_VISIBLE_NODES; }
        
        // Curtain statistics (for UI/debug)
        size_t GetTotalCurtainVertices() const;
        size_t GetTotalCurtainIndices() const;
        size_t GetTotalBaseVertices() const;
        
    private:
        // Maximum number of terrain nodes that can be visible at once
        // Used for constant buffer allocation
        static constexpr int MAX_VISIBLE_NODES = 256;
        
        TerrainQuadtree mQuadtree;
        TerrainMesh mMesh;
        TerrainRenderer mRenderer;
        
        float mWorldSize = TERRAIN_WORLD_SIZE;
        float mHeightScale = TERRAIN_HEIGHT_SCALE;
        
        bool mInitialized = false;
    };
    
}
