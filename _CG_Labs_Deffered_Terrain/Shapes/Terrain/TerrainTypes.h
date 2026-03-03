#pragma once

#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>
#include <string>
#include <cstdint>

namespace Terrain
{
    // Конфигурация террейна: размер мира, высота, LOD, сетка, занавесы, пороги расстояний, индексы текстур
    
    // Размеры в мировых координатах
    constexpr float TERRAIN_WORLD_SIZE = 1000.0f;
    constexpr float TERRAIN_HEIGHT_SCALE = 100.0f;   // Maximum height in meters
    constexpr float TERRAIN_MIN_HEIGHT = -300.0f;   // Minimum height for AABB (below sea level)
    constexpr float TERRAIN_MAX_HEIGHT = 300.0f;
    
    // LOD: три уровня, кол-во тайлов по стороне для каждого
    constexpr int MAX_LOD_LEVELS = 3;
    constexpr int LOD0_TILES = 1;   // 1x1
    constexpr int LOD1_TILES = 2;   // 2x2
    constexpr int LOD2_TILES = 4;   // 4x4
    
    // Сетка тайла: вершин по стороне (65x65)
    constexpr int GRID_SIZE = 65;
    constexpr int GRID_INDEX_COUNT = (GRID_SIZE - 1) * (GRID_SIZE - 1) * 6; // Triangle list
    
    // Шторы конфиг
    constexpr float CURTAIN_HEIGHT_FACTOR = 0.1f;     // Multiplier for curtain depth relative to max terrain height
    constexpr float CURTAIN_EXTENSION = 50.0f;        // How far curtains extend vertically downward (in world units)
    
    // LOD distance thresholds (in meters)
    constexpr float LOD0_DISTANCE = 3000.0f;         // Use LOD0 beyond this distance
    constexpr float LOD1_DISTANCE = 1500.0f;         // Use LOD1 between LOD1_DISTANCE and LOD0_DISTANCE
    // Below LOD1_DISTANCE use LOD2 (highest detail)
    
    // Heightmap array indices
    // LOD0: index 0 (1 texture)
    // LOD1: indices 1-4 (4 textures, 2x2)
    // LOD2: indices 5-20 (16 textures, 4x4)
    constexpr int HEIGHTMAP_LOD0_START = 0;
    constexpr int HEIGHTMAP_LOD1_START = 1;
    constexpr int HEIGHTMAP_LOD2_START = 5;
    constexpr int TOTAL_HEIGHTMAPS = 21;    // 1 + 4 + 16
    
    // Вершина террейна: только UV; позиция в мире вычисляется в вершинном шейдере по Heightmap
    struct TerrainVertex
    {
        DirectX::XMFLOAT2 UV;  // [0..1] в пределах патча
    };
    
    // Нода квадродерева (тайл): границы AABB, позиция и размер в XZ, LOD, индекс карты высот, четверо детей
    struct TerrainNode
    {
        DirectX::BoundingBox Bounds;     // AABB в мировых координатах для отсечения
        DirectX::XMFLOAT2 MinXZ;         // Левый нижний угол в XZ
        float Size;                       // Длина стороны в мировых единицах
        int LOD;                          // 0,1,2
        int HeightmapIndex;               // Индекс в массиве карт высот
        TerrainNode* Children[4];         // nullptr у LOD2
        
        TerrainNode()
            : MinXZ{0.0f, 0.0f}
            , Size(0.0f)
            , LOD(0)
            , HeightmapIndex(0)
        {
            Bounds.Center = {0.0f, 0.0f, 0.0f};
            Bounds.Extents = {0.0f, 0.0f, 0.0f};
            Children[0] = Children[1] = Children[2] = Children[3] = nullptr;
        }
        
        bool IsLeaf() const
        {
            return Children[0] == nullptr;
        }
    };
    
    // Константный буфер на каждый отрисовываемый тайл террейна (обновляется для каждого видимого тайла)
    struct TerrainDrawCB
    {
        DirectX::XMFLOAT2 NodeMinXZ;      // World-space offset of this node
        float NodeSize;                    // Side length of this node
        int LOD;                           // LOD level
        
        float HeightScale;                 // Height multiplier
        int HeightmapIndex;                // Index into heightmap texture array
        int EnableCurtains;                // Flag to enable/disable curtains for this draw // DEL
        float CurtainHeight;               // Height of curtains for this node
        
        // Heightmap UV transform (for this node's portion of the heightmap)
        DirectX::XMFLOAT2 HeightmapMinUV;  // UV offset into heightmap
        DirectX::XMFLOAT2 HeightmapSizeUV; // UV scale for this node
        
        // Curtain edge flags (bitmask: 0=left, 1=right, 2=top, 3=bottom)
        int CurtainEdges;                  // Which edges need curtains // DEL
        float CurtainExtension;             // Horizontal extension distance for curtains // DEL
        DirectX::XMFLOAT2 Padding2;        // Align to 16 bytes
    };
    
    
    struct TerrainTextureInfo
    {
        int LOD;
        int TileX;           // X index within LOD grid (0 for LOD0)
        int TileY;           // Y index within LOD grid (0 for LOD0)
        std::wstring HeightmapPath;
        std::wstring AlbedoPath;
        std::wstring NormalPath;
        std::wstring AOPath;
    };
    
    //Хелперы
    
    // Calculate heightmap index from LOD and tile coordinates
    inline int GetHeightmapIndex(int lod, int tileX, int tileY)
    {
        if (lod == 0) return HEIGHTMAP_LOD0_START;
        if (lod == 1) return HEIGHTMAP_LOD1_START + tileY * 2 + tileX;
        if (lod == 2) return HEIGHTMAP_LOD2_START + tileY * 4 + tileX;
        return 0;
    }
    
    // Calculate LOD level based on distance from camera
    inline int GetLODFromDistance(float distance)
    {
        if (distance >= LOD0_DISTANCE) return 0;
        if (distance >= LOD1_DISTANCE) return 1;
        return 2;
    }
    
    // Get number of tiles per side for a given LOD
    inline int GetTilesPerSide(int lod)
    {
        switch (lod)
        {
        case 0: return LOD0_TILES;
        case 1: return LOD1_TILES;
        case 2: return LOD2_TILES;
        default: return 1;
        }
    }
    
    // Get tile size for a given LOD
    inline float GetTileSize(int lod)
    {
        return TERRAIN_WORLD_SIZE / static_cast<float>(GetTilesPerSide(lod));
    }
    
    // Get curtain height for a given LOD
    inline float GetCurtainHeight()
    {
        return TERRAIN_HEIGHT_SCALE * CURTAIN_HEIGHT_FACTOR;
    }
    
}
