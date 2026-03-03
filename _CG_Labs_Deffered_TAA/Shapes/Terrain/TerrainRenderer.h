#pragma once

#include "TerrainTypes.h"
#include "TerrainMesh.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <string>

struct FrameResource;
template<typename T> class UploadBuffer;
class TerrainQuadtree;

namespace Terrain
{
    // Рендерер террейна: корневая подпись, PSO, загрузка текстур (высоты, альбедо, нормали), куча SRV, отрисовка видимых узлов.
    
    class TerrainRenderer
    {
    public:
        TerrainRenderer();
        ~TerrainRenderer();
        
        // Initialize GPU resources
        // device: D3D12 device
        // cmdList: Command list for initial setup commands
        // albedoFormat, normalFormat, velocityFormat: G-Buffer formats
        // depthFormat: Format of the depth buffer
        void Initialize(ID3D12Device* device,
                        ID3D12GraphicsCommandList* cmdList,
                        DXGI_FORMAT albedoFormat,
                        DXGI_FORMAT normalFormat,
                        DXGI_FORMAT velocityFormat,
                        DXGI_FORMAT depthFormat);
        
        // Build the terrain root signature
        void BuildRootSignature(ID3D12Device* device);
        
        // Build the terrain PSO
        void BuildPSO(ID3D12Device* device,
                      DXGI_FORMAT rtvFormat0,  // Albedo G-Buffer
                      DXGI_FORMAT rtvFormat1,  // Normal G-Buffer
                      DXGI_FORMAT rtvFormat2,  // NEW: Velocity G-Buffer
                      DXGI_FORMAT dsvFormat);
        
        // Load terrain textures from disk
        void LoadTextures(ID3D12Device* device,
                          ID3D12GraphicsCommandList* cmdList,
                          const std::wstring& basePath);
        
        // Build the SRV descriptor heap for terrain textures
        void BuildSrvHeap(ID3D12Device* device);
        
        // Draw visible terrain nodes
        // cmdList: Command list for draw calls
        // nodes: List of visible terrain nodes from quadtree
        // passCBAddress: GPU address of pass constant buffer
        // terrainCB: Upload buffer for per-draw terrain constants
        void Draw(ID3D12GraphicsCommandList* cmdList,
                  const std::vector<TerrainNode*>& nodes,
                  D3D12_GPU_VIRTUAL_ADDRESS passCBAddress,
                  UploadBuffer<TerrainDrawCB>* terrainCB);
        
        // Get root signature for external use (e.g., for debugging)
        ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }
        ID3D12PipelineState* GetPSO() const { return mPSO.Get(); }
        
        // Set the mesh to render
        void SetMesh(TerrainMesh* mesh) { mMesh = mesh; }
        
    private:
        // Check if a node needs seam rendering based on neighbor LOD levels
        bool NeedsSeam(const TerrainNode* node, int dx, int dz, const TerrainQuadtree& quadtree) const;
        // Compile terrain shaders
        void CompileShaders();
        
        // Load a single texture from file
        void LoadTexture(ID3D12Device* device,
                         ID3D12GraphicsCommandList* cmdList,
                         const std::wstring& path,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& uploadHeap);
        
    private:
        // Root signature and PSO
        Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> mPSO;
        // Depth SRV range removed (no direct depth binding here)
        
        // Shader bytecode
        Microsoft::WRL::ComPtr<ID3DBlob> mVSBytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> mPSBytecode;
        
        // Input layout for terrain vertices
        std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
        
        // Texture resources
        // Heightmaps: LOD0 (1) + LOD1 (4) + LOD2 (16) = 21 textures
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mHeightmaps;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mHeightmapUploaders;
        
        // Albedo textures (same structure as heightmaps)
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mAlbedoMaps;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mAlbedoUploaders;
        
        // Normal maps
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mNormalMaps;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mNormalUploaders;
        
        // SRV descriptor heap for terrain textures
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSrvHeap;
        UINT mSrvDescriptorSize = 0;
        
        // Reference to mesh (owned by TerrainSystem)
        TerrainMesh* mMesh = nullptr;
        
        // Formats
        DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT mDepthFormat = DXGI_FORMAT_D32_FLOAT;
    };
    
}
