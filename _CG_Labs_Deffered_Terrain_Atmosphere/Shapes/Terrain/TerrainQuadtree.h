#pragma once

#include "TerrainTypes.h"
#include <DirectXCollision.h>
#include <vector>
#include <memory>

namespace Terrain
{
    // Квадродерево террейна: иерархия тайлов, отсечение по пирамиде видимости и выбор LOD по расстоянию до камеры (на CPU).
    
    class TerrainQuadtree
    {
    public:
        TerrainQuadtree();
        ~TerrainQuadtree();
        
        // Build the quadtree for the entire terrain
        // worldSize: side length of terrain (e.g., 5000m)
        // maxLOD: deepest LOD level (e.g., 2 for LOD0/1/2)
        void Build(float worldSize, int maxLOD);
        
        // Perform frustum culling and LOD selection
        // Returns list of visible leaf nodes that should be rendered
        void Cull(const DirectX::BoundingFrustum& frustum,
                  const DirectX::XMFLOAT3& cameraPos);
        
        // Get the visible nodes after culling (these are the nodes to render)
        const std::vector<TerrainNode*>& GetVisibleNodes() const { return mVisibleNodes; }
        
        // Get culling stats
        int GetTotalNodes() const { return mTotalNodes; }
        int GetVisibleNodeCount() const { return static_cast<int>(mVisibleNodes.size()); }
        
        // LOD distance thresholds (can be modified at runtime)
        void SetLODDistances(float lod0Dist, float lod1Dist);
        float GetLOD0Distance() const { return mLOD0Distance; }
        float GetLOD1Distance() const { return mLOD1Distance; }
        
    private:
        // Recursively build quadtree nodes
        void BuildNode(TerrainNode* node, float minX, float minZ, float size, int lod, int maxLOD);
        
        // Recursively free quadtree nodes
        void FreeNode(TerrainNode* node);
        
        // Recursive culling traversal
        void CullNode(TerrainNode* node,
                      const DirectX::BoundingFrustum& frustum,
                      const DirectX::XMFLOAT3& cameraPos);
        
        // Calculate appropriate LOD for a node based on camera distance
        int SelectLOD(const TerrainNode* node, const DirectX::XMFLOAT3& cameraPos) const;
        
        // Compute AABB for a terrain node
        DirectX::BoundingBox ComputeNodeBounds(float minX, float minZ, float size) const;
        
    private:
        TerrainNode* mRoot = nullptr;
        std::vector<TerrainNode*> mVisibleNodes;
        
        int mTotalNodes = 0;
        int mMaxLOD = 2;
        float mWorldSize = TERRAIN_WORLD_SIZE;
        
        // LOD distance thresholds
        float mLOD0Distance = LOD0_DISTANCE;
        float mLOD1Distance = LOD1_DISTANCE;
    };
    
}
