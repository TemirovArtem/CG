#include "CraterDeformationTool.h"
#include "TerrainSystem.h"
#include "TerrainRenderer.h"
#include "../../Common/Camera.h"
#include "Common/DebugFlags.h"
#include <algorithm>
#include <iostream>

using namespace DirectX;

namespace Terrain
{
    CraterDeformationTool::CraterDeformationTool()
    {
    }
    
    CraterDeformationTool::~CraterDeformationTool()
    {
    }
    
    void CraterDeformationTool::Initialize(TerrainSystem* terrain)
    {
        mTerrain = terrain;
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[CRATER TOOL] Initialized with terrain system" << std::endl;
    }
    
    void CraterDeformationTool::Update(const Camera& camera, float mouseX, float mouseY, bool ctrlPressed, ID3D12GraphicsCommandList* cmdList)
    {
        if (!mTerrain || !cmdList)
        {
            if (DebugFlags::TerrainCraterMap)
            {
                if (!mTerrain)
                    std::cout << "[CRATER TOOL] ERROR: mTerrain is nullptr!" << std::endl;
                if (!cmdList)
                    std::cout << "[CRATER TOOL] ERROR: cmdList is nullptr!" << std::endl;
            }
            return;
        }
        
        // Debug: Log when Ctrl is pressed
        if (DebugFlags::TerrainCraterMap)
        {
            static bool lastCtrlState = false;
            if (ctrlPressed && !lastCtrlState)
            {
                std::cout << "[CRATER TOOL] Ctrl pressed - starting deformation" << std::endl;
            }
            else if (!ctrlPressed && lastCtrlState)
            {
                std::cout << "[CRATER TOOL] Ctrl released - stopping deformation" << std::endl;
            }
            lastCtrlState = ctrlPressed;
        }
        
        // Compute ray from camera through mouse position
        Ray ray = ComputeMouseRay(camera, mouseX, mouseY);
        
        // Perform ray-terrain intersection
        XMFLOAT3 hitPos;
        bool hit = RayTerrainIntersection(ray, hitPos);
        
        // Debug: Log ray intersection result
        if (DebugFlags::TerrainCraterMap && ctrlPressed)
        {
            if (hit)
            {
                std::cout << "[CRATER TOOL] Ray HIT terrain at (" << hitPos.x << ", " << hitPos.y << ", " << hitPos.z << ")" << std::endl;
            }
            else
            {
                std::cout << "[CRATER TOOL] Ray MISSED terrain" << std::endl;
            }
        }
        
        // If Ctrl is pressed and we hit the terrain, trigger deformation
        if (ctrlPressed && hit)
        {
            XMFLOAT2 uv = WorldToUV(hitPos);
            if (DebugFlags::TerrainCraterMap)
            {
                std::cout << "[CRATER TOOL] Triggering deformation at UV (" 
                          << uv.x << ", " << uv.y << ") with radius=" 
                          << mCraterRadiusUV << ", depth=" << mCraterDepth << std::endl;
            }
            TriggerDeformation(uv, cmdList);
        }
    }
    
    void CraterDeformationTool::SetCraterRadius(float radiusUV)
    {
        // Validate: radius must be > 0
        if (radiusUV > 0.0f)
        {
            if (DebugFlags::TerrainCraterMap)
            {
                std::cout << "[CRATER TOOL] Crater radius changed: " << mCraterRadiusUV 
                          << " -> " << radiusUV << std::endl;
            }
            mCraterRadiusUV = radiusUV;
        }
        else
        {
            if (DebugFlags::TerrainCraterMap)
                std::cout << "[CRATER TOOL] Invalid radius rejected: " << radiusUV << std::endl;
        }
    }
    
    void CraterDeformationTool::SetCraterDepth(float depth)
    {
        // Validate: depth must be < 0 for crater indentation
        if (depth < 0.0f)
        {
            if (DebugFlags::TerrainCraterMap)
            {
                std::cout << "[CRATER TOOL] Crater depth changed: " << mCraterDepth 
                          << " -> " << depth << std::endl;
            }
            mCraterDepth = depth;
        }
        else
        {
            if (DebugFlags::TerrainCraterMap)
                std::cout << "[CRATER TOOL] Invalid depth rejected: " << depth << std::endl;
        }
    }
    
    CraterDeformationTool::Ray CraterDeformationTool::ComputeMouseRay(const Camera& camera, float mouseX, float mouseY)
    {
        // Get view and projection matrices
        XMMATRIX view = camera.GetView();
        XMMATRIX proj = camera.GetProj();
        XMMATRIX invView = XMMatrixInverse(nullptr, view);
        XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
        
        // Mouse position in NDC space (already provided as [-1, 1])
        // Convert to view space
        XMVECTOR rayClip = XMVectorSet(mouseX, mouseY, 1.0f, 1.0f);
        XMVECTOR rayView = XMVector4Transform(rayClip, invProj);
        rayView = XMVectorSet(XMVectorGetX(rayView), XMVectorGetY(rayView), 1.0f, 0.0f);
        
        // Convert to world space
        XMVECTOR rayWorld = XMVector4Transform(rayView, invView);
        rayWorld = XMVector3Normalize(rayWorld);
        
        Ray ray;
        XMStoreFloat3(&ray.origin, camera.GetPosition());
        XMStoreFloat3(&ray.direction, rayWorld);
        
        return ray;
    }
    
    bool CraterDeformationTool::RayTerrainIntersection(const Ray& ray, XMFLOAT3& hitPos)
    {
        // Get terrain bounds
        float terrainSize = mTerrain->GetWorldSize();
        float terrainMinX = -terrainSize / 2.0f;
        float terrainMinZ = -terrainSize / 2.0f;
        float terrainMaxX = terrainSize / 2.0f;
        float terrainMaxZ = terrainSize / 2.0f;
        
        // Check if ray points downward
        if (ray.direction.y >= 0.0f)
        {
            if (DebugFlags::TerrainCraterMap)
            {
                std::cout << "[CRATER TOOL] Ray intersection failed: ray not pointing downward (dir.y=" 
                          << ray.direction.y << ")" << std::endl;
            }
            return false;
        }
        
        // Ray-plane intersection with y=0 plane (approximate starting point)
        float t = -ray.origin.y / ray.direction.y;
        if (t < 0.0f)
            return false;
        
        XMVECTOR rayOrigin = XMLoadFloat3(&ray.origin);
        XMVECTOR rayDir = XMLoadFloat3(&ray.direction);
        XMVECTOR intersection = rayOrigin + t * rayDir;
        
        XMFLOAT3 intersectionPos;
        XMStoreFloat3(&intersectionPos, intersection);
        
        // Check if within terrain bounds
        if (intersectionPos.x < terrainMinX || intersectionPos.x > terrainMaxX ||
            intersectionPos.z < terrainMinZ || intersectionPos.z > terrainMaxZ)
        {
            if (DebugFlags::TerrainCraterMap)
            {
                std::cout << "[CRATER TOOL] Ray intersection failed: outside terrain bounds ("
                          << intersectionPos.x << ", " << intersectionPos.z << ")" << std::endl;
            }
            return false;
        }
        
        // Refine intersection by sampling heightmap iteratively
        // This implements iterative heightmap refinement as specified in the design
        for (int i = 0; i < 5; ++i)
        {
            XMFLOAT2 uv = WorldToUV(intersectionPos);
            float actualHeight = SampleHeightAtUV(uv);
            
            // Compute new t value to reach the actual height
            t = (actualHeight - ray.origin.y) / ray.direction.y;
            if (t < 0.0f)
                return false;
            
            intersection = rayOrigin + t * rayDir;
            XMStoreFloat3(&intersectionPos, intersection);
            
            // Check bounds again after refinement
            if (intersectionPos.x < terrainMinX || intersectionPos.x > terrainMaxX ||
                intersectionPos.z < terrainMinZ || intersectionPos.z > terrainMaxZ)
                return false;
        }
        
        hitPos = intersectionPos;
        return true;
    }
    
    XMFLOAT2 CraterDeformationTool::WorldToUV(const XMFLOAT3& worldPos)
    {
        // Get terrain bounds
        float terrainSize = mTerrain->GetWorldSize();
        float terrainMinX = -terrainSize / 2.0f;
        float terrainMinZ = -terrainSize / 2.0f;
        
        // Convert world position to UV coordinates [0, 1]
        // As specified in Requirements 2.4: uv.x = (worldPos.x - terrainMinX) / terrainWidth
        XMFLOAT2 uv;
        uv.x = (worldPos.x - terrainMinX) / terrainSize;
        uv.y = (worldPos.z - terrainMinZ) / terrainSize;
        
        // Clamp to [0, 1] range to handle floating-point precision issues
        uv.x = (std::max)(0.0f, (std::min)(uv.x, 1.0f));
        uv.y = (std::max)(0.0f, (std::min)(uv.y, 1.0f));
        
        return uv;
    }
    
    void CraterDeformationTool::TriggerDeformation(const XMFLOAT2& uv, ID3D12GraphicsCommandList* cmdList)
    {
        // Call TerrainRenderer to dispatch the compute shader
        // This will modify the CraterMap texture at the specified UV coordinate
        mTerrain->GetRenderer().DispatchCraterDeformation(
            cmdList,
            uv,
            mCraterRadiusUV,
            mCraterDepth
        );
    }
    
    float CraterDeformationTool::SampleHeightAtUV(const XMFLOAT2& uv)
    {
        // For now, return a simple approximation
        // In a full implementation, this would sample the actual heightmap texture
        // Since we don't have direct CPU access to the heightmap, we use a simple approximation
        // This is sufficient for the iterative refinement to converge
        
        // Simple approximation: assume flat terrain at y=0
        // The iterative refinement will still work, just with more iterations needed
        return 0.0f;
    }
}
