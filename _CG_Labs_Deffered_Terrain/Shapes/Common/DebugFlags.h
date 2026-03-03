#pragma once

// Global debug output flags
// These can be toggled at runtime via ImGui interface
namespace DebugFlags
{
    // Terrain system debug flags
    extern bool TerrainCraterMap;      // Crater deformation system
    extern bool TerrainQuadtree;       // Quadtree culling and LOD
    extern bool TerrainRendering;      // Terrain rendering pipeline
    
    // Lighting system debug flags
    extern bool LightingSystem;        // Light calculations and updates
    extern bool ShadowMapping;         // Shadow map generation
    
    // General system debug flags
    extern bool CameraSystem;          // Camera movement and updates
    extern bool FrameResources;        // Frame resource management
    extern bool ResourceLoading;       // Texture and model loading
    
    // Performance debug flags
    extern bool PerformanceMetrics;    // FPS, frame time, etc.
    extern bool GPUTimings;            // GPU command execution times
}
