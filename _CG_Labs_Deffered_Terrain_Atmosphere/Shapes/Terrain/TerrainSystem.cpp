// ============================================================================
// TERRAIN SYSTEM - Главный оркестратор системы террейна
// ============================================================================
// 
// Этот класс является единой точкой входа для работы с террейном.
// Он координирует работу трёх основных компонентов:
//   1. TerrainQuadtree - пространственная иерархия и отсечение
//   2. TerrainMesh - универсальная сетка для всех узлов
//   3. TerrainRenderer - GPU-ресурсы и отрисовка
//
// ЖИЗНЕННЫЙ ЦИКЛ:
//   1. Initialize() - вызывается один раз при старте приложения
//   2. BuildPSO() - вызывается после создания G-Buffer (нужны форматы RTV)
//   3. Update() - вызывается каждый кадр в ShapesApp::Update()
//   4. Draw() - вызывается каждый кадр в ShapesApp::DrawOpaqueScene()
//
// ============================================================================
#include <iostream>
#include "TerrainSystem.h"
#include "../../../Common/Camera.h"
#include "../../../Common/UploadBuffer.h"
#include "Common/DebugFlags.h"

using namespace DirectX;

namespace Terrain
{
    TerrainSystem::TerrainSystem()
        : mWorldSize(TERRAIN_WORLD_SIZE)
        , mHeightScale(TERRAIN_HEIGHT_SCALE)
        , mInitialized(false)
    {
    }
    
    TerrainSystem::~TerrainSystem()
    {
    }
    
    // ========================================================================
    // ИНИЦИАЛИЗАЦИЯ СИСТЕМЫ ТЕРРЕЙНА
    // ========================================================================
    // Вызывается ОДИН РАЗ при старте приложения в ShapesApp::Initialize()
    //
    // ПОСЛЕДОВАТЕЛЬНОСТЬ ИНИЦИАЛИЗАЦИИ:
    //   1. Построение квадродерева - создание иерархии узлов (LOD 0, 1, 2)
    //   2. Построение меша - генерация универсальной сетки 65x65 с занавесами
    //   3. Инициализация рендерера - компиляция шейдеров, корневая подпись
    //   4. Установка меша в рендерер - связывание меша с рендерером
    //   5. Загрузка текстур - чтение DDS файлов (высоты, альбедо, нормали)
    //   6. Построение кучи SRV - создание дескрипторов для всех текстур
    //
    // ВАЖНО: BuildPSO() вызывается ПОСЛЕ этой функции, т.к. нужны форматы G-Buffer
    // ========================================================================

    // 
    void TerrainSystem::Initialize(ID3D12Device* device,
                                    ID3D12GraphicsCommandList* cmdList,
                                    const std::wstring& textureBasePath)
    {
        mQuadtree.Build(mWorldSize, MAX_LOD_LEVELS - 1);
        
        // Cетки для тайлов
        mMesh.Build(device, cmdList, GRID_SIZE);
        
        // Инитим рендерер
        mRenderer.Initialize(device, cmdList, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT);
        
        // Задаем наш меш для тайлов в рендерере
        mRenderer.SetMesh(&mMesh);
        
        // Загружает 21 текстуру каждого типа (height, albedo, normal):
        //   - LOD 0: 1 тайл (индекс 0)
        //   - LOD 1: 4 тайла 2x2 (индексы 1-4)
        //   - LOD 2: 16 тайлов 4x4 (индексы 5-20)
        // In Total: 63 штуки
        mRenderer.LoadTextures(device, cmdList, textureBasePath);
        
        // Initialize crater deformation system BEFORE BuildSrvHeap
        // Use 1024x1024 for CraterMap (matches typical heightmap resolution)
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN SYSTEM] Initializing crater deformation system..." << std::endl;
        mRenderer.InitializeCraterMap(device, cmdList, 1024, 1024);
        if (DebugFlags::TerrainCraterMap)
            std::cout << "[TERRAIN SYSTEM] Crater deformation system initialized" << std::endl;
        
        // Build SRV heap AFTER CraterMap is created (порядок в куче - height, cratermap, albedo, normal)
        mRenderer.BuildSrvHeap(device);
        
        mInitialized = true;
    }
    
    // ========================================================================
    // ПОСТРОЕНИЕ PSO (Pipeline State Object)
    // ========================================================================
    // Вызывается ПОСЛЕ Initialize() и ПОСЛЕ создания G-Buffer в ShapesApp
    // Нужны форматы целевых буферов (альбедо, нормаль) и глубины
    //
    // ВАЖНО: Эта функция должна вызываться после BuildGBuffer() в ShapesApp,
    //        т.к. нужны актуальные форматы RTV для настройки PSO
    // ========================================================================
    void TerrainSystem::BuildPSO(ID3D12Device* device,
                                  DXGI_FORMAT albedoFormat,
                                  DXGI_FORMAT normalFormat,
                                  DXGI_FORMAT depthFormat)
    {
        mRenderer.BuildPSO(device, albedoFormat, normalFormat, depthFormat);
    }
    
    // ========================================================================
    // ОБНОВЛЕНИЕ КАЖДЫЙ КАДР
    // ========================================================================
    // Вызывается каждый кадр в ShapesApp::Update() ПЕРЕД отрисовкой
    //
    // ЧТО ДЕЛАЕТ:
    //   1. Получает пирамиду видимости камеры (BoundingFrustum)
    //   2. Получает позицию камеры в мире
    //   3. Выполняет отсечение по пирамиде и выбор LOD по расстоянию
    //   4. Результат сохраняется в mQuadtree.mVisibleNodes
    //
    // АЛГОРИТМ:
    //   - Рекурсивный обход квадродерева от корня
    //   - Для каждого узла: проверка пересечения AABB с пирамидой
    //   - Если узел виден: выбор желаемого LOD по расстоянию до камеры
    //   - Если нужен более высокий LOD: рекурсия к детям
    //   - Иначе: добавление узла в список видимых
    //
    // ПОРЯДОК ВЫЗОВА: Update() → Draw()
    // ========================================================================
    void TerrainSystem::Update(const Camera& camera, bool mDebugCulling, float FOVFrustum)
    {
        if (!mInitialized) return;
        
        // Получение пирамиды видимости из матриц вида и проекции камеры
        BoundingFrustum frustum = mDebugCulling
            ? camera.CreateFrustumWithFovScale(FOVFrustum)
            : camera.CreateFrustum();
        
        // Позиция камеры нужна для выбора LOD по расстоянию
        XMFLOAT3 cameraPos = camera.GetPosition3f();
        
        // Отсечение и выбор LOD - результат в mQuadtree.GetVisibleNodes()
        mQuadtree.Cull(frustum, cameraPos);
    }
    
    // ========================================================================
    // ОТРИСОВКА ТЕРРЕЙНА
    // ========================================================================
    // Вызывается каждый кадр в ShapesApp::DrawOpaqueScene() 
    // ПОСЛЕ отрисовки непрозрачных объектов, но В ТОМ ЖЕ геометрическом проходе
    //
    // КОГДА ВЫЗЫВАЕТСЯ:
    //   - В геометрическом проходе отложенного рендеринга
    //   - G-Buffer уже установлен как целевые буферы (2 RTV)
    //   - Глубина уже настроена
    //
    // ЧТО ДЕЛАЕТ:
    //   1. Получает список видимых узлов из квадродерева (результат Update())
    //   2. Для каждого узла:
    //      - Заполняет TerrainDrawCB (позиция, размер, LOD, индекс текстуры)
    //      - Копирует CB в upload-буфер
    //      - Привязывает CB и SRV текстур к корневой подписи
    //      - Вызывает DrawIndexedInstanced (один вызов на узел)
    //
    // РЕЗУЛЬТАТ: Альбедо и нормали записаны в G-Buffer (SV_Target0, SV_Target1)
    //
    // ТРЕБОВАНИЯ:
    //   - Pass CB должен быть уже заполнен и привязан
    //   - Terrain CB должен быть выделен в FrameResource (размер = MAX_VISIBLE_NODES)
    // ========================================================================
    void TerrainSystem::Draw(ID3D12GraphicsCommandList* cmdList,
                              D3D12_GPU_VIRTUAL_ADDRESS passCBAddress,
                              UploadBuffer<TerrainDrawCB>* terrainCB)
    {
        if (!mInitialized || !terrainCB) return;
        
        // Получение списка видимых узлов (заполняется в Update())
        const auto& visibleNodes = mQuadtree.GetVisibleNodes();
        
        if (visibleNodes.empty()) return;
        
        // Делегирование отрисовки рендереру
        // Рендерер установит PSO, корневую подпись, буферы и отрисует все узлы
        mRenderer.Draw(cmdList, visibleNodes, passCBAddress, terrainCB);
    }
    
    int TerrainSystem::GetVisibleNodeCount() const
    {
        return mQuadtree.GetVisibleNodeCount();
    }
    
    int TerrainSystem::GetTotalNodeCount() const
    {
        return mQuadtree.GetTotalNodes();
    }
    
    void TerrainSystem::SetWorldSize(float size)
    {
        mWorldSize = size;
    }
    
    // Пороги расстояний для переключения LOD (дальше — LOD0, ближе — LOD1, ещё ближе — LOD2)
    void TerrainSystem::SetLODDistances(float lod0, float lod1)
    {
        mQuadtree.SetLODDistances(lod0, lod1);
    }
    
    float TerrainSystem::GetLOD0Distance() const
    {
        return mQuadtree.GetLOD0Distance();
    }
    
    float TerrainSystem::GetLOD1Distance() const
    {
        return mQuadtree.GetLOD1Distance();
    }
    
    // Статистика занавесов (для отладки/UI)
    size_t TerrainSystem::GetTotalCurtainVertices() const
    {
        return mMesh.GetCurtainVertexCount();
    }
    
    size_t TerrainSystem::GetTotalCurtainIndices() const
    {
        return mMesh.GetCurtainIndexCount();
    }
    
    size_t TerrainSystem::GetTotalBaseVertices() const
    {
        return mMesh.GetBaseVertexCount();
    }
    
}
