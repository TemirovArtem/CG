#include "DebugFlags.h"

// Initialize all debug flags to false by default
// Modified: Force recompilation
namespace DebugFlags
{
    bool TerrainCraterMap = false;
    bool TerrainQuadtree = false;
    bool TerrainRendering = false;
    

    bool LightingSystem = false;
    bool ShadowMapping = false;
    
    bool CameraSystem = false;
    bool FrameResources = false;
    bool ResourceLoading = false;
    
    bool PerformanceMetrics = false;
    bool GPUTimings = false;
}
