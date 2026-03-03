#include "TerrainQuadtree.h"
#include "../Common/DebugFlags.h"
#include <cmath>
#include <algorithm>
#include <iostream>

using namespace DirectX;

namespace Terrain
{
    TerrainQuadtree::TerrainQuadtree()
        : mRoot(nullptr)
        , mTotalNodes(0)
        , mMaxLOD(2)
        , mWorldSize(TERRAIN_WORLD_SIZE)
        , mLOD0Distance(LOD0_DISTANCE)
        , mLOD1Distance(LOD1_DISTANCE)
    {
    }
    
    TerrainQuadtree::~TerrainQuadtree()
    {
        if (mRoot)
        {
            FreeNode(mRoot);
            mRoot = nullptr;
        }
    }
    
    // Построение дерева: корень покрывает весь террейн, рекурсивное разбиение до maxLOD
    void TerrainQuadtree::Build(float worldSize, int maxLOD)
    {
        if (mRoot)
        {
            FreeNode(mRoot);
            mRoot = nullptr;
        }
        
        mWorldSize = worldSize;
        mMaxLOD = maxLOD;
        mTotalNodes = 0;
        
        // Террейн центрирован в начале координат: от -worldSize/2 до +worldSize/2
        float halfSize = worldSize * 0.5f;
        
        mRoot = new TerrainNode();
        BuildNode(mRoot, -halfSize, -halfSize, worldSize, 0, maxLOD);
    }
    
    void TerrainQuadtree::BuildNode(TerrainNode* node, float minX, float minZ, float size, int lod, int maxLOD)
    {
        mTotalNodes++;
        
        // Заполнение базовых свойств тайла
        node->MinXZ = XMFLOAT2(minX, minZ); // левый нижний угол в мировых коордах
        node->Size = size;
        node->LOD = lod;
        
        // Вычисление AABB для отсечения по фрустуму
        // Центр по XZ в середине тайла, по Y - среднее между min и max высотой террейна
        node->Bounds = ComputeNodeBounds(minX, minZ, size);
        
        // Определение индекса heightmap для этого тайла
        // Индекс зависит от LOD и позиции узла в сетке тайлов
        int tilesPerSide = GetTilesPerSide(lod);  // LOD 0: 1, LOD 1: 2, LOD 2: 4
        float tileSize = mWorldSize / static_cast<float>(tilesPerSide);
        float halfWorld = mWorldSize * 0.5f;
        
        // Получаем индекс тайла из позиции тайла в мире
        int tileX = static_cast<int>((minX + halfWorld) / tileSize);
        int tileY = static_cast<int>((minZ + halfWorld) / tileSize);
        
        // Ограничение индексов допустимым диапазоном
        tileX = (std::max)(0, (std::min)(tileX, tilesPerSide - 1));
        tileY = (std::max)(0, (std::min)(tileY, tilesPerSide - 1));
        
        // Вычисление индекса в массиве текстур
        // LOD 0: индекс 0, LOD 1: индексы 1-4, LOD 2: индексы 5-20
        node->HeightmapIndex = GetHeightmapIndex(lod, tileX, tileY);
        
        // Если не достигли максимального LOD - создаём дочерние тайлы
        if (lod < maxLOD)
        {
            float childSize = size * 0.5f;  // Размер дочернего узла в 2 раза меньше
            
            // Создание 4 дочерних узлов
            node->Children[0] = new TerrainNode();  // Нижний левый
            node->Children[1] = new TerrainNode();  // Нижний правый
            node->Children[2] = new TerrainNode();  // Верхний левый
            node->Children[3] = new TerrainNode();  // Верхний правый
            
            // Рекурсивное построение дочерних узлов с увеличенным LOD
            BuildNode(node->Children[0], minX, minZ, childSize, lod + 1, maxLOD);
            BuildNode(node->Children[1], minX + childSize, minZ, childSize, lod + 1, maxLOD);
            BuildNode(node->Children[2], minX, minZ + childSize, childSize, lod + 1, maxLOD);
            BuildNode(node->Children[3], minX + childSize, minZ + childSize, childSize, lod + 1, maxLOD);
        }
    }
    
    // Рекурсивное освобождение тайла и всех потомков
    void TerrainQuadtree::FreeNode(TerrainNode* node)
    {
        if (!node) return;
        
        for (int i = 0; i < 4; ++i)
        {
            if (node->Children[i])
            {
                FreeNode(node->Children[i]);
                node->Children[i] = nullptr;
            }
        }
        
        delete node;
    }
    
    // AABB тайла в мировых координатах (центр и полуразмеры по X, Y, Z)
    BoundingBox TerrainQuadtree::ComputeNodeBounds(float minX, float minZ, float size) const
    {
        BoundingBox bounds;
        
        float centerX = minX + size * 0.5f;
        float centerY = (TERRAIN_MIN_HEIGHT + TERRAIN_MAX_HEIGHT) * 0.5f;
        float centerZ = minZ + size * 0.5f;
        
        bounds.Center = XMFLOAT3(centerX, centerY, centerZ);
        
        float extentX = size * 0.5f;
        float extentY = (TERRAIN_MAX_HEIGHT - TERRAIN_MIN_HEIGHT) * 0.5f;
        float extentZ = size * 0.5f;
        
        bounds.Extents = XMFLOAT3(extentX, extentY, extentZ);
        
        return bounds;
    }
    
    void TerrainQuadtree::Cull(const BoundingFrustum& frustum, const XMFLOAT3& cameraPos)
    {
        mVisibleNodes.clear(); // обнуляем ранее извстные видимые тайлы
        
        if (DebugFlags::TerrainQuadtree)
        {
            std::cout << "[QUADTREE] Starting culling with " << mTotalNodes << " total nodes\n";
        }
        
        // Рекурсивно побегаем дерево и кулим тайлы
        if (mRoot)
        {
            CullNode(mRoot, frustum, cameraPos);
        }
        
        if (DebugFlags::TerrainQuadtree)
        {
            std::cout << "[QUADTREE] Culling complete: " << mVisibleNodes.size() << " nodes visible\n"; // скок видимых нод остается
        }
    }
    
    void TerrainQuadtree::CullNode(TerrainNode* node,
                                    const BoundingFrustum& frustum,
                                    const XMFLOAT3& cameraPos)
    {
        if (!node) return;
        
        // Проверка пересечения AABB узла с фрустумом
        ContainmentType ct = frustum.Contains(node->Bounds);
        
        static int depth = 0;
        std::string indent(depth * 2, ' ');
        
        if (DebugFlags::TerrainQuadtree)
        {
            std::cout << "[QUADTREE] " << indent << "Testing Node LOD=" << node->LOD 
                      << " Pos=(" << node->MinXZ.x << "," << node->MinXZ.y << ")" 
                      << " Size=" << node->Size;
        }
        
        if (ct == DISJOINT)
        {
            if (DebugFlags::TerrainQuadtree)
            {
                std::cout << " -> REJECTED (outside frustum)\n"; // фул вне фрустума - скип
            }
            return;
        }
        else if (ct == CONTAINS)
        {
            if (DebugFlags::TerrainQuadtree)
            {
                std::cout << " -> FULLY CONTAINED\n"; // фул внутри фрустума - оставляем
            }
        }
        else
        {
            if (DebugFlags::TerrainQuadtree)
            {
                std::cout << " -> INTERSECTS\n"; // пересекает - обращаемся к детям
            }
        }
        
        // Выбор желаемого LOD по расстоянию до камеры
        int desiredLOD = SelectLOD(node, cameraPos);
        
        if (DebugFlags::TerrainQuadtree)
        {
            std::cout << "[QUADTREE] " << indent << "  Desired LOD: " << desiredLOD << " (current: " << node->LOD << ")\n";
        }
        
        // Принятие решения о добавлении узла или рекурсии к детям
        if (node->IsLeaf())
        {
            // Лист - больше детализации нет, всегда добавляем
            mVisibleNodes.push_back(node);
            if (DebugFlags::TerrainQuadtree)
            {
                std::cout << "[QUADTREE] " << indent << "  -> ADDED (leaf node)\n";
            }
        }
        else if (node->LOD >= desiredLOD)
        {
            // Текущий LOD достаточен - добавляем этот тайл, НЕ спускаемся к детям
            mVisibleNodes.push_back(node);
            if (DebugFlags::TerrainQuadtree)
            {
                std::cout << "[QUADTREE] " << indent << "  -> ADDED (LOD threshold met)\n";
            }
        }
        else
        {
            // Нужна большая детализация - рекурсия к дочерним тайлам
            if (DebugFlags::TerrainQuadtree)
            {
                std::cout << "[QUADTREE] " << indent << "  -> RECURSING to children\n";
            }
            depth++;
            for (int i = 0; i < 4; ++i)
            {
                if (node->Children[i])
                {
                    CullNode(node->Children[i], frustum, cameraPos);
                }
            }
            depth--;
        }
    }

    int TerrainQuadtree::SelectLOD(const TerrainNode* node, const XMFLOAT3& cameraPos) const
    {
        // Вычисление расстояния от камеры до центра тайла
        float dx = node->Bounds.Center.x - cameraPos.x;
        float dy = node->Bounds.Center.y - cameraPos.y;
        float dz = node->Bounds.Center.z - cameraPos.z;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        
        // Выбор LOD по порогам расстояния
        if (distance >= mLOD0Distance) return 0;    // >= 3000м → LOD 0
        if (distance >= mLOD1Distance) return 1;    // >= 1500м → LOD 1
        return 2;                                   // < 1500м → LOD 2 (максимальная детализация)
    }
    
    void TerrainQuadtree::SetLODDistances(float lod0Dist, float lod1Dist)
    {
        mLOD0Distance = lod0Dist;
        mLOD1Distance = lod1Dist;
    }
    
}
