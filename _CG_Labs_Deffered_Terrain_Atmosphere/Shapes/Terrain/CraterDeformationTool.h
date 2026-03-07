#pragma once

#include <DirectXMath.h>
#include <d3d12.h>

class Camera;

namespace Terrain
{
    class TerrainSystem;
    
    // Manages user interaction for crater deformation
    // Handles ray-terrain intersection, Ctrl key input, and triggers compute shader deformation
    class CraterDeformationTool
    {
    public:
        CraterDeformationTool();
        ~CraterDeformationTool();
        
        // Initialize the tool with a reference to the terrain system
        // terrain: Pointer to the terrain system (must remain valid for tool lifetime)
        void Initialize(TerrainSystem* terrain);
        
        // Update the tool each frame
        // camera: Current camera for ray casting
        // mouseX: Mouse X position in normalized device coordinates [-1, 1]
        // mouseY: Mouse Y position in normalized device coordinates [-1, 1]
        // ctrlPressed: Whether the Ctrl key is currently pressed
        // cmdList: Command list for dispatching crater deformation
        void Update(const Camera& camera, float mouseX, float mouseY, bool ctrlPressed, ID3D12GraphicsCommandList* cmdList);
        
        // Configuration methods
        void SetCraterRadius(float radiusUV);
        void SetCraterDepth(float depth);
        float GetCraterRadius() const { return mCraterRadiusUV; }
        float GetCraterDepth() const { return mCraterDepth; }
        
    private:
        // Ray structure for ray-terrain intersection
        struct Ray
        {
            DirectX::XMFLOAT3 origin;
            DirectX::XMFLOAT3 direction;
        };
        
        // Compute a ray from the camera through the mouse position
        // camera: Current camera
        // mouseX: Mouse X in NDC [-1, 1]
        // mouseY: Mouse Y in NDC [-1, 1]
        // Returns: Ray in world space
        Ray ComputeMouseRay(const Camera& camera, float mouseX, float mouseY);
        
        // Perform ray-terrain intersection with iterative heightmap refinement
        // ray: Ray in world space
        // hitPos: Output world-space hit position
        // Returns: true if intersection found, false otherwise
        bool RayTerrainIntersection(const Ray& ray, DirectX::XMFLOAT3& hitPos);
        
        // Convert world-space position to UV coordinates
        // worldPos: World-space position
        // Returns: UV coordinates in [0, 1] range
        DirectX::XMFLOAT2 WorldToUV(const DirectX::XMFLOAT3& worldPos);
        
        // Trigger crater deformation at the specified UV coordinate
        // uv: UV coordinate in [0, 1] range
        // cmdList: Command list for dispatching the compute shader
        void TriggerDeformation(const DirectX::XMFLOAT2& uv, ID3D12GraphicsCommandList* cmdList);
        
        // Sample height at a given UV coordinate (for ray intersection refinement)
        // uv: UV coordinate in [0, 1] range
        // Returns: Height value at the UV coordinate
        float SampleHeightAtUV(const DirectX::XMFLOAT2& uv);
        
        TerrainSystem* mTerrain = nullptr;
        float mCraterRadiusUV = 0.02f;  // Default 2% of terrain in UV space
        float mCraterDepth = -2.0f;      // Default 2 units down
    };
}
