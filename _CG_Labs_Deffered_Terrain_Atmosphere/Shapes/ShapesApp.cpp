

#include <unordered_set>

#include <assimp/Importer.hpp>    // C++ importer interface
#include <assimp/scene.h>         // Output data structure
#include <assimp/postprocess.h>   // Post processing flags
#include <../../Common/imgui/imgui.h>
#include <DirectXCollision.h>
#include <cfloat>
#include <algorithm>
#include <functional>

#include <windows.h>
#include <iostream>
#include <ctime>
#include <random>

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/Camera.h"
#include "../../Common/Camera.cpp"
#include "Common/DebugFlags.h"
#include "FrameResource.h"
#include "Atmosphere.h"
#include "Terrain/TerrainSystem.h"
#include "Terrain/CraterDeformationTool.h"
#include <../../Common/imgui/backends/imgui_impl_win32.h>
#include <../../Common/imgui/backends/imgui_impl_dx12.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;
const float FOVFrustum = 0.5f;

// Данные загруженной модели: меши, имена материалов, цвета, пути к текстурам (диффуз, нормаль)
struct ModelData {
    std::string filename;
    std::vector<GeometryGenerator::MeshData> meshes;  // Геометрия подмешей
    std::vector<std::string> materialNames;           // Имя материала для meshes[i]
    std::vector<XMFLOAT4> diffuseColors;              // diffuse Kd цвет для каждого меша (r,g,b,a=1.0)
    std::vector<std::pair<std::string, std::string>> texturePaths;  // {materialName, diffusePath}
    std::vector<std::pair<std::string, std::string>> normalTexturePaths;  // {materialName, normalPath}
    std::vector<std::string> submeshNames;
};

// Убирает расширение и опциональный суффикс _lod01/_lod02 из имени файла модели
static std::string GetModelStem(const std::string& filename)
{
    std::string stem = filename;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot); // отрезаем .obj
    // remove _lod01 or _lod02 suffix if present
    auto pos = stem.rfind("_lod"); // ищем суффикс _lod
    if (pos != std::string::npos && stem.size() >= pos + 5) // если суффикс _lod01 или _lod02 ->
    {
        std::string tail = stem.substr(pos);
        if (tail == "_lod01" || tail == "_lod02")
            stem = stem.substr(0, pos); // -> то отрезаем суффикс _lod01 или _lod02
    }
    return stem; // возвращаем имя модели без суффикса _lod01 или _lod02 и .obj
}

// G-Buffer отложенного рендеринга: текстуры альбедо и нормалей, RTV/SRV, форматы
struct GBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Albedo;
    Microsoft::WRL::ComPtr<ID3D12Resource> Normal;

    // RTV (CPU)
    CD3DX12_CPU_DESCRIPTOR_HANDLE AlbedoRTV{};
    CD3DX12_CPU_DESCRIPTOR_HANDLE NormalRTV{};

    // SRV (CPU)
    CD3DX12_CPU_DESCRIPTOR_HANDLE AlbedoSRV{};
    CD3DX12_CPU_DESCRIPTOR_HANDLE NormalSRV{};

    // Таблица SRV (GPU) для Lighting pass
    D3D12_GPU_DESCRIPTOR_HANDLE TableGPU{};

    UINT Width = 0;
    UINT Height = 0;
    DXGI_FORMAT AlbedoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT NormalFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    UINT SrvOffset = 0;
};

// Включение консоли и перенаправление stdout/stderr/stdin (для отладочного вывода)
void EnableConsole()
{
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    std::ios::sync_with_stdio(); // для std::cout и printf
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();

}

// Загрузка модели через Assimp: триангуляция, нормали, касательные, переворот UV; выход — вершины и индексы
bool LoadModel(const std::string& path,
    std::vector<GeometryGenerator::Vertex>& vertices,
    std::vector<std::uint32_t>& indices)
{
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path,
        aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_CalcTangentSpace | aiProcess_FlipUVs);


    vertices.clear();
    indices.clear();

    if (!scene || !scene->HasMeshes()) {
        std::cerr << "Assimp error: " << importer.GetErrorString() << std::endl;
        return false;
    }
    for (size_t m = 0; m < scene->mNumMeshes; m++)
    {
        const aiMesh* mesh = scene->mMeshes[m];

        vertices.reserve(mesh->mNumVertices); // заранее выделяем память под нужное количество вершин

        // Сохраняем смещение для текущего меша
        std::uint32_t vertexOffset = static_cast<std::uint32_t>(vertices.size());


        // Врешины
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            aiVector3D pos = mesh->mVertices[i];
            aiVector3D normal = mesh->HasNormals() ? mesh->mNormals[i] : aiVector3D(0.0f, 1.0f, 0.0f);
            aiVector3D tangent = mesh->HasTangentsAndBitangents() ? mesh->mTangents[i] : aiVector3D(1.0f, 0.0f, 0.0f);
            aiVector3D uv = mesh->HasTextureCoords(0) ? mesh->mTextureCoords[0][i] : aiVector3D(0.0f, 0.0f, 0.0f);

            GeometryGenerator::Vertex v(
                DirectX::XMFLOAT3(pos.x, pos.y, pos.z),
                DirectX::XMFLOAT3(normal.x, normal.y, normal.z),
                DirectX::XMFLOAT3(tangent.x, tangent.y, tangent.z),
                DirectX::XMFLOAT2(uv.x, uv.y)
            );

            vertices.push_back(v);
        }

        // Индексы
        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            const aiFace& face = mesh->mFaces[i];
            if (face.mNumIndices != 3) continue;
            indices.push_back(vertexOffset + face.mIndices[0]);
            indices.push_back(vertexOffset + face.mIndices[1]);
            indices.push_back(vertexOffset + face.mIndices[2]);


            /*for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                indices.push_back(face.mIndices[j] + vertexOffset);
            }*/
        }
    }
    return true;
}


struct LODEntry
{
    MeshGeometry* Geo = nullptr;
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
    DirectX::BoundingBox BoundsLocal = {}; // локальный AABB для этого LOD
};


struct RenderItem
{
    RenderItem() = default;

    // имя объекта
    std::string Name;
    bool Visible = true;
    bool isDebug = false;
    int LightIndex = 0;

    // Параметры трансформации
    XMFLOAT3 Pos = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 Rot = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 World = MathHelper::Identity4x4();

    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

    // Dirty flag indicating the object data has changed and we need to update the constant buffer.
    // Because we have an object cbuffer for each FrameResource, we have to apply the
    // update to each FrameResource.  Thus, when we modify obect data we should set 
    // NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
    int NumFramesDirty = gNumFrameResources;

    // Index into GPU constant buffer corresponding to the ObjectCB for this render item.
    UINT ObjCBIndex = -1;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;

    // Local-space AABB of the submesh this render item draws
    DirectX::BoundingBox BoundsLocal = {};
    // Cached world-space AABB (computed during culling)
    DirectX::BoundingBox BoundsWorld = {};

    // LOD support
    std::vector<LODEntry> LODs; // LOD0..n (0 - highest detail) // набор уровней детализации для этого объекта
    int CurrentLOD = 0; // активный индекс LOD, который сейчас используется в отрисовке.

    // Пересчёт мировой матрицы из позиции, поворота и масштаба
    virtual void UpdateWorld()
    {
        XMMATRIX T = XMMatrixTranslation(Pos.x, Pos.y, Pos.z);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(Rot.x, Rot.y, Rot.z);
        XMMATRIX S = XMMatrixScaling(Scale.x, Scale.y, Scale.z);

        XMStoreFloat4x4(&World, S * R * T);
        NumFramesDirty = gNumFrameResources; // чтобы обновить CB
    }
};

struct LightningRenderItem : public RenderItem
{
    Light* lightObject = nullptr;
    void UpdateWorld() override
    {
        if (!lightObject) return;
        lightObject->Position = Pos;

        if (lightObject->Type == 1) // точечный
        {
            float s = (std::max)(0.1f, lightObject->FalloffEnd);
            Scale = XMFLOAT3(s, s, s);
            XMMATRIX S = XMMatrixScaling(Scale.x, Scale.y, Scale.z);
            XMMATRIX R = XMMatrixRotationRollPitchYaw(Rot.x, Rot.y, Rot.z);
            XMMATRIX T = XMMatrixTranslation(Pos.x, Pos.y, Pos.z);
            XMStoreFloat4x4(&World, S * R * T);
        }
        else if (lightObject->Type == 2) // прожектор
        {
            XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&lightObject->Direction));
            XMVECTOR up = XMVectorSet(0, 1, 0, 0);
            XMVECTOR axis = XMVector3Cross(up, d);
            float angle = XMVectorGetX(XMVector3AngleBetweenNormals(up, d));
            XMMATRIX Rdir = XMMatrixRotationAxis(axis, angle);

            float len = (std::max)(0.1f, lightObject->FalloffEnd);
            float spot = lightObject->SpotPower;
            if (spot < 0.0f) spot = 0.0f;
            if (spot > 0.9999f) spot = 0.9999f;
            float radius = (std::max)(0.1f, lightObject->FalloffEnd * tanf(acosf(spot)));
            Scale = XMFLOAT3(radius, len, radius);
            XMMATRIX S = XMMatrixScaling(Scale.x, Scale.y, Scale.z);
            XMMATRIX T = XMMatrixTranslation(Pos.x, Pos.y, Pos.z);
            XMStoreFloat4x4(&World, S * Rdir * T);
        }
        else
        {
            // направленный или неизвестный — маленький gizmo
            float s = 0.5f;
            Scale = XMFLOAT3(s, s, s);
            XMMATRIX S = XMMatrixScaling(Scale.x, Scale.y, Scale.z);
            XMMATRIX R = XMMatrixRotationRollPitchYaw(Rot.x, Rot.y, Rot.z);
            XMMATRIX T = XMMatrixTranslation(Pos.x, Pos.y, Pos.z);
            XMStoreFloat4x4(&World, S * R * T);
        }

        NumFramesDirty = gNumFrameResources;
    }
};

class ShapesApp : public D3DApp
{
public:
    ShapesApp(HINSTANCE hInstance);
    ShapesApp(const ShapesApp& rhs) = delete;
    ShapesApp& operator=(const ShapesApp& rhs) = delete;
    ~ShapesApp();

    virtual bool Initialize()override;

private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void UpdateCamera(const GameTimer& gt);
    void UpdateObjectCBs(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);

    // Logical separation for better architecture
    void UpdateScene(const GameTimer& gt);       // Culling, LOD selection, scene logic
    void UpdateFrameConstants(const GameTimer& gt); // All CB updates in one place
    void DrawOpaqueScene(ID3D12GraphicsCommandList* cmdList); // Geometry pass
    void DrawLightingPass(ID3D12GraphicsCommandList* cmdList); // Lighting pass
    void DrawDebugOverlays(ID3D12GraphicsCommandList* cmdList); // Debug wireframes

    void BuildDescriptorHeaps();
    void BuildConstantBufferViews();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildShapeGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void DrawWireframeRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

    void BuildMaterials();
    void UpdateMaterialCBs(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);

    void InitLights(UINT objCBIndex);
    void LoadTextures();
    void BuildGBuffer();
    void UpdateGBufferSrvs();
    void BuildSrvHeap();

    void LoadModels(const std::vector<std::string>& paths);
    ModelData LoadModel(const std::string& path);
    void LoadLodModels(const std::vector<std::string>& basePaths);

    void ShootLight();
    
    // Atmosphere methods
    void BuildAtmosphere();
    void UpdateAtmosphereCB(const GameTimer& gt);
    void DrawSky(ID3D12GraphicsCommandList* cmdList);

	// Frustum culling
	void UpdateFrustumCulling();

	// LOD selection
	void UpdateLODSelection();

	// Octree culling + LOD
	void BuildOctree();
	void UpdateOctreeCullingAndLOD();
	void UpdateAllWorldBounds();

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

private:
    UINT lightsCount; // количество источников света

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;
    ComPtr<ID3D12DescriptorHeap> mGBufferRtvHeap;

    ComPtr<ID3D12DescriptorHeap> mImGuiDescriptorHeap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
    std::vector<std::pair<std::string, std::unique_ptr<Texture>>> mTextureVector; // упорядочен текстуры
    std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mWireframeInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mOpaqueRitems;
    std::vector<RenderItem*> mScreenQuadRitems;
    std::vector<RenderItem*> mWireframeRitems;  // Список для wireframe-объектов

    PassConstants mMainPassCB;

    UINT mPassCbvOffset = 0;

	bool mIsWireframe = false;

    // Culling stats
    int mCulledCount = 0;
    int mVisibleCount = 0;

	// Debug culling: narrow frustum without changing camera render projection
	bool mDebugCulling = false;

	// Simple Octree
	struct OctreeNode
	{
		DirectX::BoundingBox Bounds;
		std::vector<RenderItem*> Items; // assigned by item world-aabb center
		std::unique_ptr<OctreeNode> Children[8];
		int Depth = 0;
		bool IsLeaf = true;
	};
	std::unique_ptr<OctreeNode> mOctreeRoot = nullptr;
	int mOctreeMaxDepth = 6;
	int mOctreeMaxItemsPerLeaf = 16; // максимальное количество объектов в листе
	bool mUseOctreeCulling = true; // максимальная глубина октодерева

	// Debug draw toggles
	bool mShowWireframeDebug = false;


    // LOD settings
    // дистанции для лодов 0 <-> 1,   1 <-> 2,   2 <-> 1
    float mLod0To1 = 15.0f;
    float mLod1To0 = 14.0f;
    float mLod1To2 = 30.0f;
    float mLod2To1 = 28.0f;

    // FPS Camera system
    Camera mCamera;

    // FPS Camera settings
    float mMoveSpeed = 250.0f;        // Normal movement speed
    float mFastMoveSpeed = 750.0f;    // Fast movement speed (with Shift)
    float mMouseSensitivity = 0.005f; // Mouse sensitivity for looking around
    bool mLeftMousePressed = false;   // Track left mouse button state

    POINT mLastMousePos;

    GBuffer mGBuffer;
    std::vector<ModelData> mModels;  // Список загруженных моделей


    std::vector<RenderItem*> mShootWireframeRitems;  // Список для стреляющих wireframe-объектов
    std::vector<XMFLOAT3> mShootLightVelocities;    // Скорости стреляющих источников
    std::vector<bool> mShootLightActive;            // Активны ли стреляющие источники
    int mNextShootLightIndex = 0;                   // Индекс следующего источника для стрельбы
    int MAX_SHOOT_LIGHTS = 0;                // Максимум стреляющих источников (16-5)
    int mOldestActiveShootLight = 0; // Индекс самого старого активного источника

    // Terrain subsystem
    std::unique_ptr<Terrain::TerrainSystem> mTerrain;
    bool mEnableTerrain = true;  // Toggle terrain rendering
    
    // Crater deformation tool
    std::unique_ptr<Terrain::CraterDeformationTool> mCraterTool;
    
    // Atmosphere system
    std::unique_ptr<Atmosphere> mAtmosphere;
    bool mEnableAtmosphere = true;  // Toggle atmosphere rendering
    RenderItem* mSkyRitem = nullptr;  // Sky dome render item
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    EnableConsole();

    try
    {
        ShapesApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

ShapesApp::ShapesApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    // Initialize FPS camera
    mCamera.SetPosition(0.0f, 2.0f, -15.0f);
    mCamera.SetYaw(0.0f);
    mCamera.SetPitch(0.0f);
    mCamera.UpdateFromYawPitch();
}

ShapesApp::~ShapesApp()
{
    //ImGui_ImplDX12_Shutdown();
    //ImGui_ImplWin32_Shutdown();
    //ImGui::DestroyContext();

    if (mImGuiDescriptorHeap != nullptr) { // Или другая проверка на init
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (md3dDevice != nullptr) FlushCommandQueue();

    if (md3dDevice != nullptr)
        FlushCommandQueue();
}
void ShapesApp::LoadModels(const std::vector<std::string>& paths) { // пробегаемся по путям 
    mModels.clear();  // Очищаем предыдущие
    for (const auto& path : paths) {
        ModelData data = LoadModel(path);
        if (!data.meshes.empty()) {
            data.filename = path.substr(path.find_last_of("/\\") + 1);  // Имя файла без пути
            mModels.push_back(data);
        }
    }
    std::cout << "Loaded " << mModels.size() << " models." << std::endl;
}

// Загружает LOD1/LOD2, если такие файлы существуют рядом с базовыми
void ShapesApp::LoadLodModels(const std::vector<std::string>& basePaths)
{
    // Формируем пути вида: <name>_lod01.obj, <name>_lod02.obj
    auto makeLodPath = [](const std::string& base, const char* suffix) -> std::string // string функция возвращает *_lod01/02.obj
        {
            // base like "Models/african_head.obj" → insert _lod01 before .obj
            size_t dot = base.find_last_of('.'); // номер позиции точки
            if (dot == std::string::npos) // если точка не найдена то fallback
                return base; // fallback
            std::string head = base.substr(0, dot); // очасть до точки
            std::string ext = base.substr(dot); // часть после точки
            return head + suffix + ext; // возвращаем "Models/african_head" + "_lod01" + ".obj";
        };

    // Вектор для всех путей, которые реально существуют
    std::vector<std::string> lodPaths;
    lodPaths.reserve(basePaths.size() * 2); // до 2 lod файлов в модели (LOD1 и LOD2)

    for (const auto& base : basePaths)
    {
        std::string lod1 = makeLodPath(base, "_lod01"); // путь к LOD1
        std::string lod2 = makeLodPath(base, "_lod02"); // путь к LOD2

        // Пытаемся открыть файл, если существует — добавляем
        auto exists = [](const std::string& p) -> bool // открываем файл и проверяем существует ли он
            {
                FILE* f = nullptr;
                if (fopen_s(&f, p.c_str(), "rb") == 0 && f) // если файл открыт то возвращаем true
                {
                    fclose(f);
                    return true;
                }
                return false; // если файл не открыт то возвращаем false
            };

        if (exists(lod1)) lodPaths.push_back(lod1); // если файл существует то добавляем в вектор
        if (exists(lod2)) lodPaths.push_back(lod2);
    }

    if (lodPaths.empty()) return; // нет LOD-файлов

    // Загружаем доп. модели и просто добавляем их в mModels следом.
    // На этапе сборки геометрии мы будем конструировать отдельные MeshGeometry для LOD1/LOD2.
    for (const auto& path : lodPaths) // пробегаемся по путям файлов
    {
        ModelData data = LoadModel(path);
        if (!data.meshes.empty()) { // если мешь не пустой
            data.filename = path.substr(path.find_last_of("/\\") + 1); // имя файла без пути
            mModels.push_back(data);
        }
    }
}

ModelData ShapesApp::LoadModel(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path,
        aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_CalcTangentSpace | aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices);

    ModelData modelData;
    modelData.meshes.reserve(scene->mNumMeshes);
    modelData.materialNames.reserve(scene->mNumMeshes);
    modelData.texturePaths.reserve(scene->mNumMeshes);
    modelData.normalTexturePaths.reserve(scene->mNumMeshes);

    if (!scene || !scene->HasMeshes()) {
        std::cerr << "Assimp error: " << importer.GetErrorString() << std::endl;
        return modelData;
    }

    // берем конкретный меш
    for (size_t m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        GeometryGenerator::MeshData meshData;

        // Загружаем его вершины из файла
        meshData.Vertices.reserve(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            aiVector3D pos = mesh->mVertices[i];
            aiVector3D normal = mesh->HasNormals() ? mesh->mNormals[i] : aiVector3D(0.0f, 1.0f, 0.0f);
            aiVector3D tangent = mesh->HasTangentsAndBitangents() ? mesh->mTangents[i] : aiVector3D(1.0f, 0.0f, 0.0f);
            aiVector3D uv = mesh->HasTextureCoords(0) ? mesh->mTextureCoords[0][i] : aiVector3D(0.0f, 0.0f, 0.0f);

            GeometryGenerator::Vertex v(
                DirectX::XMFLOAT3(pos.x, pos.y, pos.z),
                DirectX::XMFLOAT3(normal.x, normal.y, normal.z),
                DirectX::XMFLOAT3(tangent.x, tangent.y, tangent.z),
                DirectX::XMFLOAT2(uv.x, uv.y)
            );
            meshData.Vertices.push_back(v);
        }

        // Загружаем его индексы из файла
        meshData.Indices32.reserve(mesh->mNumFaces * 3);
        for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
            const aiFace& face = mesh->mFaces[i];
            if (face.mNumIndices != 3) continue;
            meshData.Indices32.push_back(face.mIndices[0]);
            meshData.Indices32.push_back(face.mIndices[1]);
            meshData.Indices32.push_back(face.mIndices[2]);
        }

        modelData.meshes.push_back(meshData);


        // Материал для этого меша
        std::string matNameStr = "default";
        std::string diffuseFullPath = "";
        std::string normalFullPath = "";
        XMFLOAT4 kdColor = { 1.0f, 1.0f, 1.0f, 1.0f };  // дефолт белый
        if (mesh->mMaterialIndex >= 0 && mesh->mMaterialIndex < (int)scene->mNumMaterials) {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            aiString matName;
            material->Get(AI_MATKEY_NAME, matName);
            matNameStr = matName.C_Str();


            // Парсим Kd (solid color)
            aiColor3D kd;
            if (material->Get(AI_MATKEY_COLOR_DIFFUSE, kd) == AI_SUCCESS) {
                kdColor = { (float)kd.r, (float)kd.g, (float)kd.b, 1.0f };
                //std::cout << "Parsed Kd for " << matNameStr << ": (" << kd.r << ", " << kd.g << ", " << kd.b << ")" << std::endl;
            }
            else {
                std::cout << "No Kd for " << matNameStr << ", using white" << std::endl;
            }


            // Извлекаем путь к diffuse текстуре
            aiString texPath;
            if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                diffuseFullPath = path.substr(0, path.find_last_of("/\\")) + "/" + texPath.C_Str();
            }
            // Если empty, fallback на дефолт White_diff.dds
            if (diffuseFullPath.empty()) {
                diffuseFullPath = path.substr(0, path.find_last_of("/\\")) + "/textures/White_diff.dds";
            }

            // Извлекаем путь к normal текстуре (Assimp использует aiTextureType_HEIGHT для normal maps)
            aiString normalPath;
            if (material->GetTexture(aiTextureType_DISPLACEMENT, 0, &normalPath) == AI_SUCCESS) {
                normalFullPath = path.substr(0, path.find_last_of("/\\")) + "/" + normalPath.C_Str();
            }
            // Если empty, fallback на дефолт sponza_fabric_ddn.dds
            if (normalFullPath.empty()) {
                normalFullPath = path.substr(0, path.find_last_of("/\\")) + "/textures/sponza_fabric_ddn.dds";
            }
        }
        modelData.submeshNames.push_back(mesh->mName.C_Str() ? mesh->mName.C_Str() : ("unnamed_mesh_" + std::to_string(m)));

        // After getting matNameStr and paths:
        std::cout << "Submesh " << m << " (name: '" << modelData.submeshNames.back() << "') "
            << "has material index " << mesh->mMaterialIndex
            << ", material name: '" << matNameStr << "'"
            << ", diffuse path: '" << diffuseFullPath << "'"
            << ", normal path: '" << normalFullPath << "'"
            << ", Kd color: (" << kdColor.x << "," << kdColor.y << "," << kdColor.z << ")" << std::endl;


        modelData.materialNames.push_back(matNameStr);
        modelData.diffuseColors.push_back(kdColor);  // сохраняем цвет
        modelData.texturePaths.emplace_back(matNameStr, diffuseFullPath);
        modelData.normalTexturePaths.emplace_back(matNameStr, normalFullPath);

        //std::cout << "Mesh " << m << ": matName=" << matNameStr << ", diffusePath=" << diffuseFullPath << ", normalPath=" << normalFullPath << std::endl;
    }

    std::cout << "Imported " << scene->mNumMeshes << " meshes with materials." << std::endl;
    return modelData;
}

void ShapesApp::UpdateMaterialCBs(const GameTimer& gt)
{
    auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
    for (auto& e : mMaterials)
    {
        // Only update the cbuffer data if the constants have changed.  If the cbuffer
        // data changes, it needs to be updated for each FrameResource.
        Material* mat = e.second.get();
        if (mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

            MaterialConstants matConstants;
            matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
            matConstants.FresnelR0 = mat->FresnelR0;
            matConstants.Roughness = mat->Roughness;
            XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));

            currMaterialCB->CopyData(mat->MatCBIndex, matConstants);

            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }
}

// Инициализация приложения: устройства, кучи, корневая подпись, шейдеры, геометрия, PSO, ресурсы кадра, текстуры, материалы, объекты, G-Buffer, террейн, камера
bool ShapesApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    // Reset the command list to prep for initialization commands.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    // Get the increment size of a descriptor in this heap type.  This is hardware specific, 
    // so we have to query this information.
    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    BuildShapeGeometry();
    LoadTextures();
    BuildMaterials();
    BuildGBuffer();
    BuildSrvHeap();
    BuildRootSignature();
    BuildShadersAndInputLayout();
	BuildRenderItems();
	// Build octree after items are created
	BuildOctree();
	

    // Инициализация TerrainSystem: 
    // Квадродерево -> Создание меша -> компил шейдеров hlsl -> RootSign -> Загрузка текстур -> SRV кучи
	if (mEnableTerrain)
	{
	    mTerrain = std::make_unique<Terrain::TerrainSystem>();
	    if (DebugFlags::TerrainCraterMap)
	        std::cout << "[SHAPES APP] Initializing terrain system..." << std::endl;
	    mTerrain->Initialize(md3dDevice.Get(), mCommandList.Get(), L"Textures/Terrain/");
	    if (DebugFlags::TerrainCraterMap)
	        std::cout << "[SHAPES APP] Terrain system initialized" << std::endl;
	    
	    // Initialize crater deformation tool
	    if (DebugFlags::TerrainCraterMap)
	        std::cout << "[SHAPES APP] Initializing crater deformation tool..." << std::endl;
	    mCraterTool = std::make_unique<Terrain::CraterDeformationTool>();
	    mCraterTool->Initialize(mTerrain.get());
	    mCraterTool->SetCraterRadius(0.02f);  // Default radius: 2% of terrain in UV space
	    mCraterTool->SetCraterDepth(-2.0f);   // Default depth: -2.0 units
	    if (DebugFlags::TerrainCraterMap)
	        std::cout << "[SHAPES APP] Crater deformation tool initialized with radius=0.02, depth=-2.0" << std::endl;
	}
	
    // Построение ресурсов кадра (включая Terrain CB для видимых узлов)
    BuildFrameResources();
    BuildDescriptorHeaps();
    BuildConstantBufferViews();
    BuildPSOs();
    
    // Инициализация атмосферы
    BuildAtmosphere();

    // PSO террейна: 2 RTV:
    //                  - SV_Target0: Альбедо (формат из mGBuffer.AlbedoFormat)
    //                  - SV_Target1: Нормаль (формат из mGBuffer.NormalFormat)
    //                  - DSV: Глубина (DXGI_FORMAT_D32_FLOAT)
    if (mTerrain)
    {
        mTerrain->BuildPSO(md3dDevice.Get(),
                           mGBuffer.AlbedoFormat,   // Формат альбедо из G-Buffer
                           mGBuffer.NormalFormat,    // Формат нормали из G-Buffer
                           DXGI_FORMAT_D32_FLOAT);   // Формат depth
    }

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

    //////   ImGui   //////

    // Создаём контекст ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();
    io.Fonts->AddFontDefault();
    io.Fonts->Build();


    // Создаём heap для ImGui (шрифт + srv)
    D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc = {};
    imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiHeapDesc.NumDescriptors = 1;
    imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    imguiHeapDesc.NodeMask = 0;
    md3dDevice->CreateDescriptorHeap(&imguiHeapDesc, IID_PPV_ARGS(&mImGuiDescriptorHeap));

    // Инициализация backends
    ImGui_ImplWin32_Init(mhMainWnd);
    ImGui_ImplDX12_Init(
        md3dDevice.Get(),
        gNumFrameResources,
        DXGI_FORMAT_R8G8B8A8_UNORM,     // формат backbuffer
        mImGuiDescriptorHeap.Get(),
        mImGuiDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
        mImGuiDescriptorHeap->GetGPUDescriptorHandleForHeapStart()
    );

    //////

    return true;
}

// Изменение размера окна: обновление матрицы проекции, размеров G-Buffer и целей рендера
void ShapesApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 10000.0f);

    mGBuffer.Width = mClientWidth;
    mGBuffer.Height = mClientHeight;
    BuildGBuffer();
    UpdateGBufferSrvs();
}

// Обновление каждый кадр: ввод, камера, сцена (отсечение, LOD), константы кадра (Object CB, Pass CB), террейн
void ShapesApp::Update(const GameTimer& gt)
{
    // ===================== IMGUI FRAME SETUP =====================
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Shapes Controls");
    ImGui::Text("FPS: %.1f", 1.0f / gt.DeltaTime());
    XMFLOAT3 camPos = mCamera.GetPosition3f();
    ImGui::Text("Camera Position: (%.2f, %.2f, %.2f)", camPos.x, camPos.y, camPos.z);
    ImGui::Text("Camera Yaw: %.2f°", XMConvertToDegrees(mCamera.GetYaw()));
    ImGui::Text("Camera Pitch: %.2f°", XMConvertToDegrees(mCamera.GetPitch()));
    ImGui::Separator();
    ImGui::Checkbox("Debug culling", &mDebugCulling);
    ImGui::Checkbox("Show Wireframe Debug", &mShowWireframeDebug);
    ImGui::Checkbox("Use Octree Culling", &mUseOctreeCulling);
    if (ImGui::Button("Rebuild Octree")) { BuildOctree(); }
    if (ImGui::CollapsingHeader("LOD Settings"))
    {
        ImGui::DragFloat("LOD0 -> LOD1 (m)", &mLod0To1, 0.1f, 0.0f, 10000.0f);
        ImGui::DragFloat("LOD1 -> LOD0 (m)", &mLod1To0, 0.1f, 0.0f, 10000.0f);
        ImGui::DragFloat("LOD1 -> LOD2 (m)", &mLod1To2, 0.1f, 0.0f, 10000.0f);
        ImGui::DragFloat("LOD2 -> LOD1 (m)", &mLod2To1, 0.1f, 0.0f, 10000.0f);
    }
    ImGui::Text("Culled: %d", mCulledCount);
    ImGui::Text("Visible: %d", mVisibleCount);
    ImGui::Separator();
    ImGui::Text("Octree: depth=%d, leafCap=%d", mOctreeMaxDepth, mOctreeMaxItemsPerLeaf);
    ImGui::SliderInt("Octree Max Depth", &mOctreeMaxDepth, 1, 10);
    ImGui::SliderInt("Octree Leaf Capacity", &mOctreeMaxItemsPerLeaf, 1, 128);
    
    
    ImGui::Separator();
    ImGui::Text("FPS Camera Controls:");
    ImGui::Text("WASD - Move camera");
    ImGui::Text("Shift - Speed boost");
    ImGui::Text("Hold LMB + Mouse - Look around");

    if (ImGui::CollapsingHeader("Camera Settings"))
    {
        ImGui::SliderFloat("Move Speed", &mMoveSpeed, 1.0f, 550.0f);
        ImGui::SliderFloat("Fast Move Speed", &mFastMoveSpeed, 10.0f, 10000.0f);
        ImGui::SliderFloat("Mouse Sensitivity", &mMouseSensitivity, 0.001f, 0.01f, "%.4f");
    }
    
    if (mCraterTool && ImGui::CollapsingHeader("Crater Deformation Tool"))
    {
        ImGui::Text("Hold Ctrl + Hover over terrain to deform");
        float radius = mCraterTool->GetCraterRadius();
        if (ImGui::SliderFloat("Crater Radius (UV)", &radius, 0.005f, 0.1f, "%.3f"))
        {
            mCraterTool->SetCraterRadius(radius);
        }
        float depth = mCraterTool->GetCraterDepth();
        if (ImGui::SliderFloat("Crater Depth", &depth, -10.0f, -0.1f, "%.1f"))
        {
            mCraterTool->SetCraterDepth(depth);
        }
    }
    
    // Debug Output Controls
    if (ImGui::CollapsingHeader("DEBUG OUTPUT"))
    {
        ImGui::Text("Enable/disable debug console output:");
        ImGui::Separator();
        
        ImGui::Text("Terrain System:");
        ImGui::Checkbox("Crater Deformation", &DebugFlags::TerrainCraterMap);
        ImGui::Checkbox("Quadtree Culling", &DebugFlags::TerrainQuadtree);
        ImGui::Checkbox("Terrain Rendering", &DebugFlags::TerrainRendering);
        
        ImGui::Separator();
        ImGui::Text("Lighting System:");
        ImGui::Checkbox("Lighting", &DebugFlags::LightingSystem);
        ImGui::Checkbox("Shadow Mapping", &DebugFlags::ShadowMapping);
        
        ImGui::Separator();
        ImGui::Text("General Systems:");
        ImGui::Checkbox("Camera", &DebugFlags::CameraSystem);
        ImGui::Checkbox("Frame Resources", &DebugFlags::FrameResources);
        ImGui::Checkbox("Resource Loading", &DebugFlags::ResourceLoading);
        
        ImGui::Separator();
        ImGui::Text("Performance:");
        ImGui::Checkbox("Performance Metrics", &DebugFlags::PerformanceMetrics);
        ImGui::Checkbox("GPU Timings", &DebugFlags::GPUTimings);
        
        ImGui::Separator();
        if (ImGui::Button("Enable All"))
        {
            DebugFlags::TerrainCraterMap = true;
            DebugFlags::TerrainQuadtree = true;
            DebugFlags::TerrainRendering = true;
            DebugFlags::LightingSystem = true;
            DebugFlags::ShadowMapping = true;
            DebugFlags::CameraSystem = true;
            DebugFlags::FrameResources = true;
            DebugFlags::ResourceLoading = true;
            DebugFlags::PerformanceMetrics = true;
            DebugFlags::GPUTimings = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Disable All"))
        {
            DebugFlags::TerrainCraterMap = false;
            DebugFlags::TerrainQuadtree = false;
            DebugFlags::TerrainRendering = false;
            DebugFlags::LightingSystem = false;
            DebugFlags::ShadowMapping = false;
            DebugFlags::CameraSystem = false;
            DebugFlags::FrameResources = false;
            DebugFlags::ResourceLoading = false;
            DebugFlags::PerformanceMetrics = false;
            DebugFlags::GPUTimings = false;
        }
    }

    /// Задаем солнцу движение относительно времени в системе

    auto& params = mAtmosphere->GetParameters();

    SYSTEMTIME st;
    GetLocalTime(&st); // локальное время в винде пользователя
    int currentSeconds = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    int totalDaySeconds = 24 * 3600;
    float timeOfDay = static_cast<float>(currentSeconds) / totalDaySeconds;
    std::printf("Seconds: %02d\n", currentSeconds);
    std::printf("Seconds in per day: %02d\n", totalDaySeconds);
    std::printf("Time of day: %02f\n", timeOfDay);

    params.SunDirection.y = sin(timeOfDay);
    params.SunDirection.z = cos(timeOfDay);

    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&params.SunDirection));
    XMStoreFloat3(&params.SunDirection, dir);

    ///

    // ========================================== ATMOSPHERE CONTROLS ==========================================
    if (mAtmosphere && ImGui::CollapsingHeader("Atmosphere Settings"))
    {
        ImGui::Checkbox("Enable Atmosphere", &mEnableAtmosphere);
        ImGui::Separator();
        

        

         
        
        // Пресеты
        ImGui::Text("Presets:");
        if (ImGui::Button("Clean Sky"))
            mAtmosphere->SetCleanAtmosphere();
        ImGui::SameLine();
        if (ImGui::Button("Polluted"))
            mAtmosphere->SetDirtyAtmosphere();
        ImGui::SameLine();
        if (ImGui::Button("Heavy Smog"))
            mAtmosphere->SetMarsAtmosphere();
        ImGui::SameLine();
        if (ImGui::Button("Sunset"))
            mAtmosphere->SetSunsetAtmosphere();
        
        ImGui::Separator();
        
        // Основные параметры
        ImGui::Text("Sun:");
        if (ImGui::DragFloat3("Direction", &params.SunDirection.x, 0.01f, -1.0f, 1.0f))
        {
            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&params.SunDirection));
            XMStoreFloat3(&params.SunDirection, dir);
        }
        ImGui::SliderFloat("Intensity", &params.SunIntensity, 5.0f, 50.0f, "%.1f");
        
        ImGui::Separator();
        ImGui::Text("Atmosphere:");
        ImGui::SliderFloat("Density", &params.DensityMultiplier, 0.1f, 5.0f, "%.2f");
        ImGui::SliderFloat("Mie Anisotropy", &params.MieAnisotropy, 0.5f, 0.95f, "%.2f");
        
        ImGui::Separator();
        ImGui::SliderFloat("Exposure", &params.Exposure, 0.5f, 3.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("sponza"))
    {
        for (int i = 0; i < mOpaqueRitems.size(); i++)
        {
            auto& ri = mOpaqueRitems[i];
            ImGui::PushID("Obj");
            ImGui::PushID(ri);

            if (ImGui::CollapsingHeader(("Object " + std::to_string(i)).c_str()))
            {
                if (ImGui::DragFloat3("Position", &ri->Pos.x, 0.1f))
                    ri->UpdateWorld();

                if (ImGui::DragFloat3("Rotation (rad)", &ri->Rot.x, 0.01f))
                    ri->UpdateWorld();

                if (ImGui::DragFloat3("Scale", &ri->Scale.x, 0.01f, 0.01f, 10.0f))
                    ri->UpdateWorld();
            }

            ImGui::PopID();
            ImGui::PopID();
        }
    }

    for (int i = 0; i < mWireframeRitems.size(); i++)
    {
        auto lightRitem = dynamic_cast<LightningRenderItem*>(mWireframeRitems[i]);
        auto& light = *lightRitem->lightObject;

        ImGui::PushID("Light");
        ImGui::PushID(lightRitem);
        if (ImGui::CollapsingHeader(("Light " + std::to_string(i)).c_str()))
        {
            ImGui::Text("Type: %s", (light.Type == 0 ? "Directional" : light.Type == 1 ? "Point" : "Spot"));

            // Общие параметры для всех источников света
            ImGui::ColorEdit3("Strength", &light.Strength.x);

            if (light.Type == 0) // Directional
            {
                if (ImGui::DragFloat3("Direction", &light.Direction.x, 0.01f, -1.0f, 1.0f))
                {
                    // нормализация
                    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&light.Direction));
                    XMStoreFloat3(&light.Direction, dir);
                }
            }
            else if (lightRitem->lightObject->Type == 1) // point
            {
                if (ImGui::DragFloat3("Position", &lightRitem->Pos.x, 0.1f))
                    lightRitem->UpdateWorld();

                if (ImGui::DragFloat("FalloffStart", &lightRitem->lightObject->FalloffStart, 0.1f, 0.0f, lightRitem->lightObject->FalloffEnd))
                    lightRitem->UpdateWorld();

                if (ImGui::DragFloat("FalloffEnd", &lightRitem->lightObject->FalloffEnd, 0.1f, 0.1f, 100.0f))
                    lightRitem->UpdateWorld();
            }
            else if (lightRitem->lightObject->Type == 2) // spot
            {
                if (ImGui::DragFloat3("Position", &lightRitem->Pos.x, 0.1f))
                    lightRitem->UpdateWorld();

                if (ImGui::DragFloat3("Direction", &lightRitem->lightObject->Direction.x, 0.01f, -1.0f, 1.0f))
                    lightRitem->UpdateWorld();

                if (ImGui::DragFloat("FalloffStart", &lightRitem->lightObject->FalloffStart, 0.1f, 0.0f, lightRitem->lightObject->FalloffEnd))
                    lightRitem->UpdateWorld();

                if (ImGui::DragFloat("FalloffEnd", &lightRitem->lightObject->FalloffEnd, 0.1f, 0.1f, 100.0f))
                    lightRitem->UpdateWorld();

                if (ImGui::DragFloat("SpotPower", &lightRitem->lightObject->SpotPower, 0.01f, 0.0f, 1.0f))
                    lightRitem->UpdateWorld();
            }

        }
        ImGui::PopID();
        ImGui::PopID();
    }


    ImGui::End();
    ImGui::Render();

    // ========================================== INPUT & CAMERA ==========================================
    OnKeyboardInput(gt);
    UpdateCamera(gt);

    // ========================================== SCENE UPDATE ==========================================
    UpdateScene(gt);

    // ========================================== FRAME RESOURCE SYNC ==========================================
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    // ========================================== CONSTANT BUFFER UPDATES ==========================================
    UpdateFrameConstants(gt);
}

// Главный цикл отрисовки: переход в G-Buffer, гео-проход (непрозрачные + террейн), световой проход по кваду, отладка (wireframe), ImGui
void ShapesApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reset command list
    ThrowIfFailed(cmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["gbuffer"].Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);
    
    // ========================================== CRATER DEFORMATION ==========================================
    // Update crater deformation tool with current input state
    if (mCraterTool && mTerrain)
    {
        // Convert mouse position from screen space to NDC
        // NDC: [-1, 1] where (-1, -1) is bottom-left and (1, 1) is top-right
        float mouseNdcX = (2.0f * mLastMousePos.x / mClientWidth) - 1.0f;
        float mouseNdcY = 1.0f - (2.0f * mLastMousePos.y / mClientHeight);
        
        // Check if Ctrl key is pressed
        bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        
        // Debug: Log input state (only when Ctrl is pressed to avoid spam)
        if (DebugFlags::TerrainCraterMap)
        {
            static bool lastCtrlState = false;
            if (ctrlPressed && !lastCtrlState)
            {
                if (DebugFlags::TerrainCraterMap)
                {
                    std::cout << "[SHAPES APP] Ctrl pressed - Mouse NDC: (" 
                              << mouseNdcX << ", " << mouseNdcY << ")" << std::endl;
                }
            }
            lastCtrlState = ctrlPressed;
        }
        
        // Update crater tool (performs ray-terrain intersection and triggers deformation if Ctrl is held)
        mCraterTool->Update(mCamera, mouseNdcX, mouseNdcY, ctrlPressed, mCommandList.Get());
    }
    else
    {
        if (DebugFlags::TerrainCraterMap)
        {
            static bool errorLogged = false;
            if (!errorLogged)
            {
                if (!mCraterTool)
                    std::cout << "[SHAPES APP] ERROR: mCraterTool is nullptr!" << std::endl;
                if (!mTerrain)
                    std::cout << "[SHAPES APP] ERROR: mTerrain is nullptr!" << std::endl;
                errorLogged = true;
            }
        }
    }

    // Backbuffer: PRESENT -> RENDER_TARGET
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // ========================================== GEOMETRY PASS ==========================================
    DrawOpaqueScene(mCommandList.Get());

    // G-Buffer: RENDER_TARGET -> SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER toSRV[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer.Albedo.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer.Normal.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    mCommandList->ResourceBarrier(_countof(toSRV), toSRV);

    // ========================================== LIGHTING PASS ==========================================
    DrawLightingPass(mCommandList.Get());
    
    // Depth: SHADER_RESOURCE -> DEPTH_WRITE (для Sky Pass)
    D3D12_RESOURCE_BARRIER toDepthWriteForSky = CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE
    );
    mCommandList->ResourceBarrier(1, &toDepthWriteForSky);
    
    // ========================================== SKY PASS ==========================================
    DrawSky(mCommandList.Get());

    // ========================================== DEBUG OVERLAYS ==========================================
    DrawDebugOverlays(mCommandList.Get());

    // ========================================== IMGUI ==========================================
    ID3D12DescriptorHeap* imguiHeaps[] = { mImGuiDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(imguiHeaps), imguiHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

    // G-Buffer: SHADER_RESOURCE -> RENDER_TARGET (for next frame)
    D3D12_RESOURCE_BARRIER toRT[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer.Albedo.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer.Normal.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
    };
    mCommandList->ResourceBarrier(_countof(toRT), toRT);

    // Backbuffer: RENDER_TARGET -> PRESENT
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Submit command list
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Present and advance fence
    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;
    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

// Геометрический проход: очистка G-Buffer и глубины, отрисовка непрозрачных объектов и террейна в альбедо/нормаль
void ShapesApp::DrawOpaqueScene(ID3D12GraphicsCommandList* cmdList)
{
    // Setup MRT: Albedo + Normal + Depth
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs = {
        mGBuffer.AlbedoRTV,
        mGBuffer.NormalRTV
    };
    auto dsv = DepthStencilView();
    cmdList->OMSetRenderTargets((UINT)rtvs.size(), rtvs.data(), FALSE, &dsv);

    // Clear G-Buffer
    cmdList->ClearRenderTargetView(mGBuffer.AlbedoRTV, Colors::Black, 0, nullptr);
    cmdList->ClearRenderTargetView(mGBuffer.NormalRTV, Colors::Black, 0, nullptr);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set PSO and root signature for regular geometry
    cmdList->SetPipelineState(mPSOs["gbuffer"].Get());
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    // Set PassCB
    cmdList->SetGraphicsRootConstantBufferView(2, mCurrFrameResource->PassCB->Resource()->GetGPUVirtualAddress());

    // Draw all opaque render items (regular meshes)
    DrawRenderItems(cmdList, mOpaqueRitems);

    // ========================================================================
    // ОТРИСОВКА ТЕРРЕЙНА В ГЕОМЕТРИЧЕСКОМ ПРОХОДЕ
    // ========================================================================
    // Вызывается в DrawOpaqueScene() ПОСЛЕ отрисовки непрозрачных объектов
    // Выполняется в ТОМ ЖЕ геометрическом проходе (G-Buffer уже установлен)
    //
    // КОГДА ВЫЗЫВАЕТСЯ:
    //   - G-Buffer уже установлен как целевые буферы (альбедо, нормаль)
    //   - Глубина уже настроена
    //   - Непрозрачные объекты уже отрисованы в G-Buffer
    //
    // ЧТО ДЕЛАЕТ:
    //   1. Устанавливает PSO террейна (свои шейдеры, корневая подпись)
    //   2. Для каждого видимого узла:
    //      - Заполняет TerrainDrawCB (позиция, размер, LOD, текстуры)
    //      - Привязывает CB и SRV текстур
    //      - Вызывает DrawIndexedInstanced
    //   3. Записывает альбедо и нормали в G-Buffer (SV_Target0, SV_Target1)
    //
    // РЕЗУЛЬТАТ: Альбедо и нормали террейна добавлены в G-Buffer
    //            Используются в световом проходе для расчёта освещения
    //
    // ВАЖНО: После отрисовки террейна нужно восстановить основной PSO,
    //        если планируется дальнейшая отрисовка в этом проходе
    // ========================================================================
    if (mTerrain && mTerrain->GetVisibleNodeCount() > 0 && mCurrFrameResource->TerrainCB)
    {
        if (DebugFlags::TerrainRendering)
        {
            std::cout << "[DEBUG] Drawing terrain with " << mTerrain->GetVisibleNodeCount() << " visible nodes\n";
        }
        
        // РИсуем террейн:
        // - Pass CB: GPU-адрес константного буфера прохода (вид, проекция, время)
        // - Terrain CB: upload-буфер для констант каждого видимого узла
        mTerrain->Draw(cmdList,
                       mCurrFrameResource->PassCB->Resource()->GetGPUVirtualAddress(),
                       mCurrFrameResource->TerrainCB.get());
        
        // Восстановление основного PSO ( для будущей последующей отрисовки )
        cmdList->SetPipelineState(mPSOs["gbuffer"].Get());
        cmdList->SetGraphicsRootSignature(mRootSignature.Get());
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        cmdList->SetGraphicsRootConstantBufferView(2, mCurrFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }
}

// Световой проход: полноэкранный квад, чтение G-Buffer (альбедо, нормаль), расчёт освещения и вывод в back buffer
void ShapesApp::DrawLightingPass(ID3D12GraphicsCommandList* cmdList)
{
    auto backRtv = CurrentBackBufferView();
    cmdList->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(backRtv, Colors::Black, 0, nullptr);

    // Set PSO and root signature
    cmdList->SetPipelineState(mPSOs["lighting"].Get());
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());

    // Set descriptor heaps
    ID3D12DescriptorHeap* lHeaps[] = { mSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(lHeaps), lHeaps);

    // Set PassCB and G-Buffer SRVs
    cmdList->SetGraphicsRootConstantBufferView(2, mCurrFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    
    // Set Atmosphere CB for atmospheric scattering on objects
    if (mCurrFrameResource->AtmosphereCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(3, mCurrFrameResource->AtmosphereCB->Resource()->GetGPUVirtualAddress());
    }
    
    cmdList->SetGraphicsRootDescriptorTable(5, mGBuffer.TableGPU); // Изменено с 4 на 5

    // Fullscreen triangle
    cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void ShapesApp::DrawDebugOverlays(ID3D12GraphicsCommandList* cmdList)
{
    if (!mShowWireframeDebug)
        return;

    cmdList->SetPipelineState(mPSOs["wireframe"].Get());
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());

    cmdList->RSSetViewports(1, &mScreenViewport);
    cmdList->RSSetScissorRects(1, &mScissorRect);

    auto backRtv = CurrentBackBufferView();
    auto dsv = DepthStencilView();
    cmdList->OMSetRenderTargets(1, &backRtv, FALSE, &dsv);

    // Set PassCB
    cmdList->SetGraphicsRootConstantBufferView(2, mCurrFrameResource->PassCB->Resource()->GetGPUVirtualAddress());

    // Force debug items visible
    for (auto* ri : mWireframeRitems) ri->Visible = true;
    for (auto* ri : mShootWireframeRitems) ri->Visible = true;

    // Draw wireframe debug items
    DrawWireframeRenderItems(cmdList, mWireframeRitems);
    DrawWireframeRenderItems(cmdList, mShootWireframeRitems);
}

void ShapesApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse)
        return; // если мышь обрабатывает ImGui, камера не двигается

    if (btnState & MK_LBUTTON)
    {
        mLeftMousePressed = true;
        mLastMousePos.x = x;
        mLastMousePos.y = y;
        SetCapture(mhMainWnd);
    }
}

void ShapesApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse)
        return; // если мышь обрабатывает ImGui, камера не двигается

    if (!(btnState & MK_LBUTTON))
    {
        mLeftMousePressed = false;
        ReleaseCapture();
    }
}

void ShapesApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse)
        return; // если мышь обрабатывает ImGui, камера не двигается

    if (mLeftMousePressed && (btnState & MK_LBUTTON) != 0)
    {
        // Calculate mouse movement delta
        float dx = static_cast<float>(mLastMousePos.x - x);
        float dy = static_cast<float>(y - mLastMousePos.y);

        // Apply mouse sensitivity
        dx *= mMouseSensitivity;
        dy *= mMouseSensitivity;

        // Update yaw and pitch
        float currentYaw = mCamera.GetYaw();
        float currentPitch = mCamera.GetPitch();

        mCamera.SetYaw(currentYaw + dx);  // Horizontal mouse movement affects yaw
        mCamera.SetPitch(currentPitch - dy); // Vertical mouse movement affects pitch (inverted)
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void ShapesApp::OnKeyboardInput(const GameTimer& gt)
{
    // Добавляем задержку между выстрелами
    static float shootCooldown = 0.0f;
    shootCooldown -= gt.DeltaTime();

    if ((GetAsyncKeyState('L') & 0x8000) && shootCooldown <= 0.0f)
    {
        ShootLight();
        shootCooldown = 0.2f; // 200ms задержка между выстрелами
    }

    // FPS Camera movement
    float deltaTime = gt.DeltaTime();
    float currentMoveSpeed = mMoveSpeed;

    // Check for speed boost (Shift key)
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
    {
        currentMoveSpeed = mFastMoveSpeed;
    }

    float moveDistance = currentMoveSpeed * deltaTime;

    // WASD movement
    if (GetAsyncKeyState('W') & 0x8000)  // Move forward
    {
        mCamera.Walk(moveDistance);
    }
    if (GetAsyncKeyState('S') & 0x8000)  // Move backward
    {
        mCamera.Walk(-moveDistance);
    }
    if (GetAsyncKeyState('A') & 0x8000)  // Strafe left
    {
        mCamera.Strafe(-moveDistance);
    }
    if (GetAsyncKeyState('D') & 0x8000)  // Strafe right
    {
        mCamera.Strafe(moveDistance);
    }
}

void ShapesApp::AnimateMaterials(const GameTimer& gt)
{

}

void ShapesApp::UpdateCamera(const GameTimer& gt)
{
    // Update camera view matrix
    mCamera.UpdateFromYawPitch();
    mCamera.UpdateViewMatrix();
}

// Обновление сцены: отсечение по пирамиде видимости (или по октодереву), выбор LOD, обновление мировых AABB
void ShapesApp::UpdateScene(const GameTimer& gt)
{
    // Scene-level updates: culling, LOD selection
    if (mUseOctreeCulling) {
        UpdateOctreeCullingAndLOD();
    }
    else {
        UpdateLODSelection();
        UpdateFrustumCulling();
    }

    // Update shoot light positions
    for (int i = 0; i < MAX_SHOOT_LIGHTS; ++i)
    {
        if (mShootLightActive[i])
        {
            mShootWireframeRitems[i]->Pos.x += mShootLightVelocities[i].x * gt.DeltaTime();
            mShootWireframeRitems[i]->Pos.y += mShootLightVelocities[i].y * gt.DeltaTime();
            mShootWireframeRitems[i]->Pos.z += mShootLightVelocities[i].z * gt.DeltaTime();
            mShootWireframeRitems[i]->UpdateWorld();
    
            // Update light position in CB
            mMainPassCB.Lights[5 + i].Position = mShootWireframeRitems[i]->Pos;
    
            XMVECTOR lightPos = XMLoadFloat3(&mShootWireframeRitems[i]->Pos);
            XMVECTOR eyePos = mCamera.GetPosition();
            float distance = XMVectorGetX(XMVector3Length(lightPos - eyePos));
    
            // DEBUG: Log active shoot lights
            if (DebugFlags::LightingSystem)
            {
                std::cout << "[LIGHTS] Shoot Light " << i << " Active Pos: (" 
                          << mShootWireframeRitems[i]->Pos.x << ", " 
                          << mShootWireframeRitems[i]->Pos.y << ", " 
                          << mShootWireframeRitems[i]->Pos.z << ") Distance: " << distance << "\n";
            }
    
            if (distance > 1000.0f)
            {
                mShootLightActive[i] = false;
                mMainPassCB.Lights[5 + i].Strength = { 0.0f, 0.0f, 0.0f };
                mShootWireframeRitems[i]->Visible = false;
    
                if (i == mOldestActiveShootLight)
                {
                    mOldestActiveShootLight = (mOldestActiveShootLight + 1) % MAX_SHOOT_LIGHTS;
                }
            }
        }
    }
    
    // ========================================================================
    // ОБНОВЛЕНИЕ ПОДСИСТЕМЫ ТЕРРЕЙНА
    // ========================================================================
    // Вызывается каждый кадр в UpdateScene() ПЕРЕД отрисовкой
    //
    // ЧТО ДЕЛАЕТ:
    //   1. Получает пирамиду видимости камеры (BoundingFrustum)
    //   2. Получает позицию камеры в мире
    //   3. Выполняет отсечение по пирамиде видимости
    //   4. Выбирает уровень детализации (LOD) по расстоянию до камеры
    //   5. Сохраняет список видимых узлов для использования в Draw()
    //
    // РЕЗУЛЬТАТ: Список видимых узлов в TerrainQuadtree::mVisibleNodes
    //            Используется в Draw() для отрисовки только видимых частей террейна
    //
    // ПРОИЗВОДИТЕЛЬНОСТЬ: Большинство узлов отсекаются на ранних уровнях дерева,
    //                     что значительно снижает количество отрисовываемых узлов
    // ========================================================================
    if (mTerrain)
    {
        mTerrain->Update(mCamera, mDebugCulling, FOVFrustum);
        if (DebugFlags::TerrainQuadtree)
        {
            std::cout << "==============================================================================\n";
            std::cout << "[DEBUG] Terrain updated, visible nodes: " << mTerrain->GetVisibleNodeCount() << "\n";
        }
    }
}

// Обновление всех константных буферов кадра: Object CB, Material CB, Pass CB; для террейна — Terrain CB в Draw
void ShapesApp::UpdateFrameConstants(const GameTimer& gt)
{
    // Update all constant buffers in one logical place
    AnimateMaterials(gt);
    UpdateMainPassCB(gt);
    UpdateMaterialCBs(gt);
    UpdateObjectCBs(gt);
    UpdateAtmosphereCB(gt);
}

void ShapesApp::UpdateObjectCBs(const GameTimer& gt)
{
    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRitems)
    {
        // Only update the cbuffer data if the constants have changed.  
        // This needs to be tracked per frame resource.
        if (e->NumFramesDirty > 0)
        {
            // Если это LightningRenderItem
            if (auto lightRitem = dynamic_cast<LightningRenderItem*>(e.get()))
            {
                lightRitem->UpdateWorld(); // обновляет World и lightObject

                XMMATRIX world = XMLoadFloat4x4(&lightRitem->World);
                XMMATRIX worldInv = XMMatrixInverse(nullptr, world);
                XMMATRIX texTransform = XMLoadFloat4x4(&lightRitem->TexTransform);

                ObjectConstants objConstants;
                XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
                XMStoreFloat4x4(&objConstants.WorldInvTranspose, XMMatrixTranspose(worldInv));
                XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

                currObjectCB->CopyData(lightRitem->ObjCBIndex, objConstants);
                e->NumFramesDirty--; // ???
            }
            else
            {
                // Обычный RenderItem
                XMMATRIX world = XMLoadFloat4x4(&e->World);
                XMMATRIX worldInv = XMMatrixInverse(nullptr, world);
                XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

                ObjectConstants objConstants;
                XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
                XMStoreFloat4x4(&objConstants.WorldInvTranspose, XMMatrixTranspose(worldInv));
                XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

                currObjectCB->CopyData(e->ObjCBIndex, objConstants);
                e->NumFramesDirty--;
            }
        }
    }
}



void ShapesApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetView();
    XMMATRIX proj = mCamera.GetProj();

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);  
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
    mMainPassCB.EyePosW = mCamera.GetPosition3f();     
    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f /  mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 10000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
    //mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
    mMainPassCB.AmbientLight = { 0.7f, 0.7f, 0.7f, 1.0f };

    // DEBUG: Log light positions
    if (DebugFlags::LightingSystem)
    {
        std::cout << "[LIGHTS] Camera Pos: (" << mMainPassCB.EyePosW.x << ", " 
                  << mMainPassCB.EyePosW.y << ", " << mMainPassCB.EyePosW.z << ")\n";
        for (int i = 0; i < lightsCount; ++i)
        {
            auto& light = mMainPassCB.Lights[i];
            if (light.Type == 1 || light.Type == 2) // Point or Spot
            {
                std::cout << "[LIGHTS] Light " << i << " (Type=" << light.Type << ") Pos: (" 
                          << light.Position.x << ", " << light.Position.y << ", " << light.Position.z 
                          << ") Strength: (" << light.Strength.x << ", " << light.Strength.y << ", " << light.Strength.z << ")\n";
            }
        }
    }

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void ShapesApp::UpdateLODSelection()
{
    XMFLOAT3 eye = mCamera.GetPosition3f();
    for (auto& e : mAllRitems)
    {
        RenderItem* ri = e.get();

        // Distance from camera to object origin
        float dx = ri->Pos.x - eye.x;
        float dy = ri->Pos.y - eye.y;
        float dz = ri->Pos.z - eye.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        int lod = ri->CurrentLOD;
        if (lod == 0)
        {
            if (dist >= mLod0To1 && ri->LODs.size() > 1)
                lod = 1;
        }
        else if (lod == 1)
        {
            if (dist < mLod1To0)
                lod = 0;
            else if (dist >= mLod1To2 && ri->LODs.size() > 2)
                lod = 2;
        }
        else if (lod == 2)
        {
            if (dist < mLod2To1)
                lod = 1;
        }

        ri->CurrentLOD = lod;

        // Switch draw args to selected LOD
        // ставим в drawargs объекта новый по индексу
        if (!ri->LODs.empty())
        {
            const LODEntry& sel = ri->LODs[ri->CurrentLOD];
            ri->Geo = sel.Geo;
            ri->IndexCount = sel.IndexCount;
            ri->StartIndexLocation = sel.StartIndexLocation;
            ri->BaseVertexLocation = sel.BaseVertexLocation;
            ri->BoundsLocal = sel.BoundsLocal;
        }
    }
}

void ShapesApp::UpdateFrustumCulling()
{
    DirectX::BoundingFrustum camFrustum = mDebugCulling
        ? mCamera.CreateFrustumWithFovScale(FOVFrustum)
        : mCamera.CreateFrustum();

    mCulledCount = 0;
    mVisibleCount = 0;

    using DirectX::XMMATRIX;
    using DirectX::BoundingBox;

    for (auto& e : mAllRitems)
    {
        RenderItem* ri = e.get();

        XMMATRIX world = XMLoadFloat4x4(&ri->World);

        BoundingBox worldBB;
        ri->BoundsLocal.Transform(worldBB, world);
        ri->BoundsWorld = worldBB;

        // DISJOINT: объём полностью вне другого (нет пересечения).
        // если ct не DISJOINT, то объект видим
        // он проверяет пересечение объёма с фрустом(если ни разу не было пересечения с плоскостями фрустума, то DISJOINT)
        DirectX::ContainmentType ct = camFrustum.Contains(worldBB);
        bool visible = (ct != DirectX::DISJOINT);
        ri->Visible = visible;
        if (visible) ++mVisibleCount; else ++mCulledCount;
    }
}

// --- Octree helpers ---
static DirectX::BoundingBox MakeBoundsFromCenterExtent(const DirectX::XMFLOAT3& c, float ex, float ey, float ez)
{
    DirectX::BoundingBox bb;
    bb.Center = c;
    bb.Extents = { ex, ey, ez };
    return bb;
}

void ShapesApp::UpdateAllWorldBounds()
{
    using DirectX::BoundingBox;
    using DirectX::XMMATRIX;
    for (auto& e : mAllRitems)
    {
        RenderItem* ri = e.get();
        XMMATRIX world = XMLoadFloat4x4(&ri->World);
        BoundingBox worldBB;
        ri->BoundsLocal.Transform(worldBB, world);
        ri->BoundsWorld = worldBB;
    }
}

static int GetChildIndexForPoint(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& p)
{
    int idx = 0;
    if (p.x >= center.x) idx |= 1;
    if (p.y >= center.y) idx |= 2;
    if (p.z >= center.z) idx |= 4;
    return idx;
}

void ShapesApp::BuildOctree()
{
    UpdateAllWorldBounds();

    if (mAllRitems.empty()) 
    { 
        mOctreeRoot.reset(); 
        return; 
    }

    // Вычисляем общий bounding box всей сцены    
    DirectX::XMFLOAT3 minPt = { FLT_MAX, FLT_MAX, FLT_MAX };
    DirectX::XMFLOAT3 maxPt = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (auto& e : mAllRitems)
    {
        const auto& b = e->BoundsWorld;
        DirectX::XMFLOAT3 c = b.Center;
        DirectX::XMFLOAT3 ex = b.Extents;
        DirectX::XMFLOAT3 bmin = { c.x - ex.x, c.y - ex.y, c.z - ex.z };
        DirectX::XMFLOAT3 bmax = { c.x + ex.x, c.y + ex.y, c.z + ex.z };
        minPt.x = (std::min)(minPt.x, bmin.x);
        minPt.y = (std::min)(minPt.y, bmin.y);
        minPt.z = (std::min)(minPt.z, bmin.z);
        maxPt.x = (std::max)(maxPt.x, bmax.x);
        maxPt.y = (std::max)(maxPt.y, bmax.y);
        maxPt.z = (std::max)(maxPt.z, bmax.z);
    }
    DirectX::XMFLOAT3 sceneCenter = { (minPt.x + maxPt.x) * 0.5f, (minPt.y + maxPt.y) * 0.5f, (minPt.z + maxPt.z) * 0.5f };
    DirectX::XMFLOAT3 sceneExtent = { (maxPt.x - minPt.x) * 0.5f, (maxPt.y - minPt.y) * 0.5f, (maxPt.z - minPt.z) * 0.5f };
    float maxEx = (std::max)(sceneExtent.x, (std::max)(sceneExtent.y, sceneExtent.z));
    sceneExtent = { maxEx, maxEx, maxEx };

    // корневой узел дерева
    mOctreeRoot = std::make_unique<OctreeNode>();
    mOctreeRoot->Bounds = MakeBoundsFromCenterExtent(sceneCenter, sceneExtent.x, sceneExtent.y, sceneExtent.z);
    mOctreeRoot->Depth = 0;
    mOctreeRoot->IsLeaf = true;

    // Кладем ВСЕ объекты в корневой узел
    for (auto& e : mAllRitems) 
        mOctreeRoot->Items.push_back(e.get());

    std::function<void(OctreeNode&)> subdivide = [&](OctreeNode& node)
    {
        // Если в узле слишком много объектов и мы не достигли максимальной глубины
        if ((int)node.Items.size() <= mOctreeMaxItemsPerLeaf || node.Depth >= mOctreeMaxDepth)
        {
            node.IsLeaf = true;
            return;
        }
        node.IsLeaf = false;
        DirectX::XMFLOAT3 c = node.Bounds.Center;
        DirectX::XMFLOAT3 e = node.Bounds.Extents;
        DirectX::XMFLOAT3 childExt = { e.x * 0.5f, e.y * 0.5f, e.z * 0.5f };

        // Делим узел на 8 частей
        for (int i = 0; i < 8; ++i)
        {
            node.Children[i] = std::make_unique<OctreeNode>();
            // Вычисляем центр и размеры каждой части
            node.Children[i]->Depth = node.Depth + 1;
            node.Children[i]->IsLeaf = true;
        }

        auto childCenter = [&](int idx) -> DirectX::XMFLOAT3
        {
            float sx = (idx & 1) ? 0.5f : -0.5f;
            float sy = (idx & 2) ? 0.5f : -0.5f;
            float sz = (idx & 4) ? 0.5f : -0.5f;
            return { c.x + sx * e.x, c.y + sy * e.y, c.z + sz * e.z };
        };

        for (int i = 0; i < 8; ++i)
        {
            node.Children[i]->Bounds = MakeBoundsFromCenterExtent(childCenter(i), childExt.x, childExt.y, childExt.z);
        }

        // Распределяем объекты по дочерним узлам
        for (auto* ri : node.Items)
        {
            // Определяем, в какую из 8 частей попадает центр объекта
            int idx = GetChildIndexForPoint(c, ri->BoundsWorld.Center);
            node.Children[idx]->Items.push_back(ri);
        }
        node.Items.clear();

        for (int i = 0; i < 8; ++i)
        {
            if (!node.Children[i]->Items.empty()) subdivide(*node.Children[i]);
        }
    };

    // Рекурсивно делим пространство
    subdivide(*mOctreeRoot);
}

void ShapesApp::UpdateOctreeCullingAndLOD()
{
    // Recompute world bounds each frame for moved items
    UpdateAllWorldBounds();

    // создаем фрустум дебаг
    DirectX::BoundingFrustum camFrustum = mDebugCulling
        ? mCamera.CreateFrustumWithFovScale(FOVFrustum)
        : mCamera.CreateFrustum();

    XMFLOAT3 eye = mCamera.GetPosition3f();

    //  Для начала все объекты как невидимые
    mVisibleCount = 0;
    for (auto& e : mAllRitems) 
        e->Visible = false;


    // обработка листовых узлов, выполняется когда узел полностью внутри фрустума, помечает все объекты в листе как видимые без дополнительных проверок
    std::function<void(OctreeNode&)> markSubtree = [&](OctreeNode& node) 
    {
        if (!node.IsLeaf)
        {
            for (int i = 0; i < 8; ++i) if (node.Children[i]) markSubtree(*node.Children[i]);
            return;
        }
        for (auto* ri : node.Items)
        {
            // Для каждого видимого объекта вычисляется расстояние от камеры до центра его AABB :
            const XMFLOAT3& c = ri->BoundsWorld.Center;
            float dx = c.x - eye.x;
            float dy = c.y - eye.y;
            float dz = c.z - eye.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);

            int lod = ri->CurrentLOD;
            if (lod == 0)
            {
                if (dist >= mLod0To1 && ri->LODs.size() > 1) lod = 1;
            }
            else if (lod == 1)
            {
                if (dist < mLod1To0) lod = 0;
                else if (dist >= mLod1To2 && ri->LODs.size() > 2) lod = 2;
            }
            else if (lod == 2)
            {
                if (dist < mLod2To1) lod = 1;
            }

            if (!ri->LODs.empty())
            {
                ri->CurrentLOD = lod;
                const LODEntry& sel = ri->LODs[ri->CurrentLOD];
                ri->Geo = sel.Geo;
                ri->IndexCount = sel.IndexCount;
                ri->StartIndexLocation = sel.StartIndexLocation;
                ri->BaseVertexLocation = sel.BaseVertexLocation;
                ri->BoundsLocal = sel.BoundsLocal;
            }

            if (!ri->Visible) { ri->Visible = true; ++mVisibleCount; }
        }
    };

    std::function<void(OctreeNode&)> visit = [&](OctreeNode& node) // Проверяет, пересекается ли узел с фрустумом камеры
    {
        DirectX::ContainmentType ct = camFrustum.Contains(node.Bounds);
        // Если узел полностью вне фрустума - пропускает всю подветвь
        if (ct == DirectX::DISJOINT) 
            return; // skip subtree

        // Если узел полностью внутри фрустума - помечает все объекты в поддереве как видимые
        if (ct == DirectX::CONTAINS)
        {
            markSubtree(node);
            return; // Узел полностью вне поля зрения - пропускаем
        }

        if (!node.IsLeaf)
        {
            for (int i = 0; i < 8; ++i) 
                if (node.Children[i]) 
                    visit(*node.Children[i]);
            return;
        }

        // Для частично видимых узлов рекурсивно проверяет дочерние узлы
        for (auto* ri : node.Items)
        {
            DirectX::ContainmentType cto = camFrustum.Contains(ri->BoundsWorld);
            if (cto == DirectX::DISJOINT) 
            { 
                ri->Visible = false; 
                continue; 
            }

            // LOD selection by distance to world AABB center (more robust than object origin)
            const XMFLOAT3& c = ri->BoundsWorld.Center;
            float dx = c.x - eye.x;
            float dy = c.y - eye.y;
            float dz = c.z - eye.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);

            int lod = ri->CurrentLOD;
            if (lod == 0)
            {
                if (dist >= mLod0To1 && ri->LODs.size() > 1) lod = 1;
            }
            else if (lod == 1)
            {
                if (dist < mLod1To0) lod = 0;
                else if (dist >= mLod1To2 && ri->LODs.size() > 2) lod = 2;
            }
            else if (lod == 2)
            {
                if (dist < mLod2To1) lod = 1;
            }

            if (!ri->LODs.empty())
            {
                ri->CurrentLOD = lod;
                const LODEntry& sel = ri->LODs[ri->CurrentLOD];
                ri->Geo = sel.Geo;
                ri->IndexCount = sel.IndexCount;
                ri->StartIndexLocation = sel.StartIndexLocation;
                ri->BaseVertexLocation = sel.BaseVertexLocation;
                ri->BoundsLocal = sel.BoundsLocal;
            }

            if (!ri->Visible) { ri->Visible = true; ++mVisibleCount; }
        }
    };

    if (mOctreeRoot) 
        visit(*mOctreeRoot);

    // Derive culled count
    mCulledCount = (int)mAllRitems.size() - mVisibleCount;
}

// Построение куч дескрипторов: CBV/SRV (объекты, материалы, pass, текстуры, G-Buffer)
void ShapesApp::BuildDescriptorHeaps()
{
    UINT objCount = (UINT)mOpaqueRitems.size(); // количество объектов на сцене
    UINT matCount = (UINT)mMaterials.size();
    UINT passCount = gNumFrameResources;

    // Need a CBV descriptor for each object for each frame resource,
    // +1 for the perPass CBV for each frame resource.
    UINT numDescriptors = objCount * gNumFrameResources
        + matCount * gNumFrameResources
        + passCount;

    // Save an offset to the start of the pass CBVs.
    // Смещение начала Pass CBV блока
    mPassCbvOffset = objCount * gNumFrameResources + matCount * gNumFrameResources;

    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
    cbvHeapDesc.NumDescriptors = numDescriptors;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&cbvHeapDesc,
        IID_PPV_ARGS(&mCbvHeap)));
}

void ShapesApp::BuildConstantBufferViews()
{
    mCurrFrameResourceIndex = 0;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

    UINT objCount = (UINT)mOpaqueRitems.size(); // количество объектов на сцене
    //UINT objCount = (UINT)mAllRitems.size();

    // Need a CBV descriptor for each object for each frame resource.
    // CPU дескриптор handle стартовый
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(mCbvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- Objects: objCount * gNumFrameResources ---
    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto objectCB = mFrameResources[frameIndex]->ObjectCB->Resource();
        for (UINT i = 0; i < objCount; ++i)
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = objectCB->GetGPUVirtualAddress() + i * objCBByteSize;
            cbvDesc.SizeInBytes = objCBByteSize;
            md3dDevice->CreateConstantBufferView(&cbvDesc, handle);
            handle.Offset(1, mCbvSrvUavDescriptorSize);
        }
    }

    // --- Materials: matCount * gNumFrameResources ---
    UINT matCount = (UINT)mMaterials.size();
    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto matCB = mFrameResources[frameIndex]->MaterialCB->Resource();
        for (UINT i = 0; i < matCount; ++i)
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = matCB->GetGPUVirtualAddress() + i * matCBByteSize;
            cbvDesc.SizeInBytes = matCBByteSize;
            md3dDevice->CreateConstantBufferView(&cbvDesc, handle);
            handle.Offset(1, mCbvSrvUavDescriptorSize);
        }
    }

    // --- Pass CBs: one per frame ---
    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto passCB = mFrameResources[frameIndex]->PassCB->Resource();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = passCB->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = passCBByteSize;
        md3dDevice->CreateConstantBufferView(&cbvDesc, handle);
        handle.Offset(1, mCbvSrvUavDescriptorSize);
    }
}

// Построение корневой подписи: слоты для CBV (pass, object, material), SRV (текстуры), статические сэмплеры
void ShapesApp::BuildRootSignature()
{
    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[6]; // Увеличено с 5 до 6 для атмосферы

    CD3DX12_DESCRIPTOR_RANGE texTable{};   // t0... — материалы (albedo/normal и т.п.)
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, /*numDescriptors=*/2, /*baseShaderRegister=*/0);

    CD3DX12_DESCRIPTOR_RANGE gbufTable{};  // t2... — G-Buffer SRV
    gbufTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, /*numDescriptors=*/3, /*baseShaderRegister=*/2);

    // Create root CBV.
    slotRootParameter[0].InitAsConstantBufferView(0); // b0 - Object CB
    slotRootParameter[1].InitAsConstantBufferView(1); // b1 - Material CB
    slotRootParameter[2].InitAsConstantBufferView(2); // b2 - Pass CB
    slotRootParameter[3].InitAsConstantBufferView(3); // b3 - Atmosphere CB
    slotRootParameter[4].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL); // t0..
    slotRootParameter[5].InitAsDescriptorTable(1, &gbufTable, D3D12_SHADER_VISIBILITY_PIXEL); // t2..

    auto staticSamplers = GetStaticSamplers();


    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter, (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void ShapesApp::BuildShadersAndInputLayout()
{
    // GBuffer pass
    mShaders["geometryVS"] = d3dUtil::CompileShader(L"Shaders\\Geometry.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["geometryPS"] = d3dUtil::CompileShader(L"Shaders\\Geometry.hlsl", nullptr, "PS", "ps_5_1");

    // Lighting pass
    mShaders["lightingVS"] = d3dUtil::CompileShader(L"Shaders\\Lighting.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["lightingPS"] = d3dUtil::CompileShader(L"Shaders\\Lighting.hlsl", nullptr, "PS", "ps_5_1");

    // Wireframe pass
    mShaders["wireframeVS"] = d3dUtil::CompileShader(L"Shaders\\Wireframe.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["wireframePS"] = d3dUtil::CompileShader(L"Shaders\\Wireframe.hlsl", nullptr, "PS", "ps_5_1");
    
    // Sky pass
    mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_1");


    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    mWireframeInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
}

void ShapesApp::BuildShapeGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
    GeometryGenerator::MeshData box = geoGen.CreateBox(1.5f, 0.5f, 1.5f, 3);
    GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
    GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);


    std::vector<std::string> modelPaths = {
        "Models/african_head.obj",  // Добавь вторую, если есть
        "Models/african_head.obj",
        "Models/african_head.obj"
    };
    LoadModels(modelPaths);
    LoadLodModels(modelPaths);


    std::vector<GeometryGenerator::MeshData> meshesVector = { /*box, sphere, cylinder,*/ };
    size_t totalMeshes = 0;
    for (const auto& model : mModels)
    {
        meshesVector.insert(meshesVector.end(), model.meshes.begin(), model.meshes.end());
        totalMeshes += model.meshes.size();
    }

    
    //
    // We are concatenating all the geometry into one big vertex/index buffer.  So
    // define the regions in the buffer each submesh covers.
    //

    // Вычисление offsets для вершин и индексов каждого объекта в meshesVector
    std::vector<UINT> vertexOffsets = { 0 };
    std::vector<UINT> indexOffsets = { 0 };
    for (size_t i = 0; i < (meshesVector.size() - 1); i++)
    {
        vertexOffsets.push_back(vertexOffsets[i] + (UINT)meshesVector[i].Vertices.size());
        indexOffsets.push_back(indexOffsets[i] + (UINT)meshesVector[i].Indices32.size());
    }

    // Define the SubmeshGeometry that cover different 
    // regions of the vertex/index buffers.

    size_t totalVertexCount = 0;
    std::vector<std::uint16_t> indices;

    // Заполнение информации для каждого submesh
    // Define the SubmeshGeometry that cover different 
    // regions of the vertex/index buffers.
    std::vector<SubmeshGeometry> submeshVector; // вектор сабмешей
    for (size_t i = 0; i < meshesVector.size(); i++)
    {
        SubmeshGeometry sGeo;
        sGeo.IndexCount = (UINT)meshesVector[i].Indices32.size();
        sGeo.StartIndexLocation = indexOffsets[i];
        sGeo.BaseVertexLocation = vertexOffsets[i];
        // Локальные AABB каждого подмеша
        using DirectX::XMFLOAT3;
        DirectX::XMFLOAT3 vmin(FLT_MAX, FLT_MAX, FLT_MAX);
        DirectX::XMFLOAT3 vmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const auto& v : meshesVector[i].Vertices)
        {
            vmin.x = (std::min)(vmin.x, v.Position.x);
            vmin.y = (std::min)(vmin.y, v.Position.y);
            vmin.z = (std::min)(vmin.z, v.Position.z);
            vmax.x = (std::max)(vmax.x, v.Position.x);
            vmax.y = (std::max)(vmax.y, v.Position.y);
            vmax.z = (std::max)(vmax.z, v.Position.z);
        }
        sGeo.Bounds.Center = XMFLOAT3(
            0.5f * (vmin.x + vmax.x),
            0.5f * (vmin.y + vmax.y),
            0.5f * (vmin.z + vmax.z));
        sGeo.Bounds.Extents = XMFLOAT3(
            0.5f * (vmax.x - vmin.x),
            0.5f * (vmax.y - vmin.y),
            0.5f * (vmax.z - vmin.z));
        submeshVector.push_back(sGeo);

        // Extract the vertex elements we are interested in and pack the
        // vertices of all the meshes into one vertex buffer.
        totalVertexCount += meshesVector[i].Vertices.size();

        indices.insert(indices.end(), std::begin(meshesVector[i].GetIndices16()), std::end(meshesVector[i].GetIndices16()));
    }

    //
    // Extract the vertex elements we are interested in and pack the
    // vertices of all the meshes into one vertex buffer.
    //
    std::vector<Vertex> vertices(totalVertexCount); // масси всех вершин и его заполнение

    UINT k = 0;
    for (size_t m = 0; m < meshesVector.size(); m++)
    {
        for (size_t i = 0; i < meshesVector[m].Vertices.size(); ++i, ++k)
        {
            vertices[k].Pos = meshesVector[m].Vertices[i].Position;
            vertices[k].Normal = meshesVector[m].Vertices[i].Normal;
            vertices[k].TexC = meshesVector[m].Vertices[i].TexC;
            vertices[k].TangentU = meshesVector[m].Vertices[i].TangentU;
        }
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "shapeGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    for (size_t i = 0; i < meshesVector.size(); i++)
    {
        geo->DrawArgs["mesh" + std::to_string(i)] = submeshVector[i];
    }

    mGeometries[geo->Name] = std::move(geo);

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // debug lights    
    GeometryGenerator::MeshData lightSphere = geoGen.CreateSphere(1.0f, 20, 20);
    GeometryGenerator::MeshData lightCone = geoGen.CreateCylinder(0.0f, 1.0f, 1.0f, 20, 20);

    std::vector<GeometryGenerator::MeshData> debugLightMeshes = { lightSphere, lightCone };

    // Вычисление offsets для вершин и индексов каждого объекта в meshesVector
    vertexOffsets = { 0 };
    indexOffsets = { 0 };
    for (size_t i = 0; i < (debugLightMeshes.size() - 1); i++)
    {
        vertexOffsets.push_back(vertexOffsets[i] + (UINT)debugLightMeshes[i].Vertices.size());
        indexOffsets.push_back(indexOffsets[i] + (UINT)debugLightMeshes[i].Indices32.size());
    }

    // Define the SubmeshGeometry that cover different 
    // regions of the vertex/index buffers.

    totalVertexCount = 0;
    std::vector<std::uint16_t> debugIndices;

    // Заполнение информации для каждого submesh
    // Define the SubmeshGeometry that cover different 
    // regions of the vertex/index buffers.
    std::vector<SubmeshGeometry> debugSubmeshVector;
    for (size_t i = 0; i < debugLightMeshes.size(); i++)
    {
        SubmeshGeometry sGeo;
        sGeo.IndexCount = (UINT)debugLightMeshes[i].Indices32.size();
        sGeo.StartIndexLocation = indexOffsets[i];
        sGeo.BaseVertexLocation = vertexOffsets[i];
        // Compute bounds for debug meshes too (optional but keeps API consistent)
        using DirectX::XMFLOAT3;
        DirectX::XMFLOAT3 vmin(FLT_MAX, FLT_MAX, FLT_MAX);
        DirectX::XMFLOAT3 vmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const auto& v : debugLightMeshes[i].Vertices)
        {
            vmin.x = (std::min)(vmin.x, v.Position.x);
            vmin.y = (std::min)(vmin.y, v.Position.y);
            vmin.z = (std::min)(vmin.z, v.Position.z);
            vmax.x = (std::max)(vmax.x, v.Position.x);
            vmax.y = (std::max)(vmax.y, v.Position.y);
            vmax.z = (std::max)(vmax.z, v.Position.z);
        }
        sGeo.Bounds.Center = XMFLOAT3(
            0.5f * (vmin.x + vmax.x),
            0.5f * (vmin.y + vmax.y),
            0.5f * (vmin.z + vmax.z));
        sGeo.Bounds.Extents = XMFLOAT3(
            0.5f * (vmax.x - vmin.x),
            0.5f * (vmax.y - vmin.y),
            0.5f * (vmax.z - vmin.z));
        debugSubmeshVector.push_back(sGeo);

        // Extract the vertex elements we are interested in and pack the
        // vertices of all the meshes into one vertex buffer.
        totalVertexCount += debugLightMeshes[i].Vertices.size();

        debugIndices.insert(debugIndices.end(), std::begin(debugLightMeshes[i].GetIndices16()), std::end(debugLightMeshes[i].GetIndices16()));
    }

    //
    // Extract the vertex elements we are interested in and pack the
    // vertices of all the meshes into one vertex buffer.
    //
    std::vector<Vertex> debugVertices(totalVertexCount);

    k = 0;
    for (size_t m = 0; m < debugLightMeshes.size(); m++)
    {
        for (size_t i = 0; i < debugLightMeshes[m].Vertices.size(); ++i, ++k)
        {
            debugVertices[k].Pos = debugLightMeshes[m].Vertices[i].Position;
            debugVertices[k].Normal = debugLightMeshes[m].Vertices[i].Normal;
        }
    }

    const UINT debugVbByteSize = (UINT)debugVertices.size() * sizeof(Vertex);
    const UINT debugIbByteSize = (UINT)debugIndices.size() * sizeof(std::uint16_t);

    auto lightGeo = std::make_unique<MeshGeometry>();
    lightGeo->Name = "lightGeo";

    ThrowIfFailed(D3DCreateBlob(debugVbByteSize, &lightGeo->VertexBufferCPU));
    CopyMemory(lightGeo->VertexBufferCPU->GetBufferPointer(), debugVertices.data(), debugVbByteSize);

    ThrowIfFailed(D3DCreateBlob(debugIbByteSize, &lightGeo->IndexBufferCPU));
    CopyMemory(lightGeo->IndexBufferCPU->GetBufferPointer(), debugIndices.data(), debugIbByteSize);

    lightGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), debugVertices.data(), debugVbByteSize, lightGeo->VertexBufferUploader);

    lightGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), debugIndices.data(), debugIbByteSize, lightGeo->IndexBufferUploader);

    lightGeo->VertexByteStride = sizeof(Vertex);
    lightGeo->VertexBufferByteSize = debugVbByteSize;
    lightGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
    lightGeo->IndexBufferByteSize = debugIbByteSize;

    for (size_t i = 0; i < debugLightMeshes.size(); i++)
    {
        lightGeo->DrawArgs["mesh" + std::to_string(i)] = debugSubmeshVector[i];
    }

    mGeometries[lightGeo->Name] = std::move(lightGeo);
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Sky sphere geometry
    GeometryGenerator::MeshData skySphere = geoGen.CreateSphere(5000.0f, 30, 30);
    
    std::vector<Vertex> skyVertices(skySphere.Vertices.size());
    for (size_t i = 0; i < skySphere.Vertices.size(); ++i)
    {
        skyVertices[i].Pos = skySphere.Vertices[i].Position;
        skyVertices[i].Normal = skySphere.Vertices[i].Normal;
        skyVertices[i].TexC = skySphere.Vertices[i].TexC;
        skyVertices[i].TangentU = skySphere.Vertices[i].TangentU;
    }
    
    std::vector<std::uint16_t> skyIndices = skySphere.GetIndices16();
    
    const UINT skyVbByteSize = (UINT)skyVertices.size() * sizeof(Vertex);
    const UINT skyIbByteSize = (UINT)skyIndices.size() * sizeof(std::uint16_t);
    
    auto skyGeo = std::make_unique<MeshGeometry>();
    skyGeo->Name = "skyGeo";
    
    ThrowIfFailed(D3DCreateBlob(skyVbByteSize, &skyGeo->VertexBufferCPU));
    CopyMemory(skyGeo->VertexBufferCPU->GetBufferPointer(), skyVertices.data(), skyVbByteSize);
    
    ThrowIfFailed(D3DCreateBlob(skyIbByteSize, &skyGeo->IndexBufferCPU));
    CopyMemory(skyGeo->IndexBufferCPU->GetBufferPointer(), skyIndices.data(), skyIbByteSize);
    
    skyGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), skyVertices.data(), skyVbByteSize, skyGeo->VertexBufferUploader);
    
    skyGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), skyIndices.data(), skyIbByteSize, skyGeo->IndexBufferUploader);
    
    skyGeo->VertexByteStride = sizeof(Vertex);
    skyGeo->VertexBufferByteSize = skyVbByteSize;
    skyGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
    skyGeo->IndexBufferByteSize = skyIbByteSize;
    
    SubmeshGeometry skySubmesh;
    skySubmesh.IndexCount = (UINT)skyIndices.size();
    skySubmesh.StartIndexLocation = 0;
    skySubmesh.BaseVertexLocation = 0;
    
    skyGeo->DrawArgs["sky"] = skySubmesh;
    
    mGeometries[skyGeo->Name] = std::move(skyGeo);
}

// Построение всех PSO: гео-проход (opaque), световой проход (quad), wireframe, террейн (вызывается после BuildRootSignature и шейдеров)
void ShapesApp::BuildPSOs()
{
    // --- PSO: GBuffer ( Albedo, Normal) ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufDesc = {};
    gbufDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    gbufDesc.pRootSignature = mRootSignature.Get();
    gbufDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["geometryVS"]->GetBufferPointer()),
        mShaders["geometryVS"]->GetBufferSize()
    };
    gbufDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["geometryPS"]->GetBufferPointer()),
        mShaders["geometryPS"]->GetBufferSize()
    };
    gbufDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gbufDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gbufDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    gbufDesc.SampleMask = UINT_MAX;
    gbufDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gbufDesc.NumRenderTargets = 2;
    gbufDesc.RTVFormats[0] = mGBuffer.AlbedoFormat;
    gbufDesc.RTVFormats[1] = mGBuffer.NormalFormat;
    gbufDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gbufDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    gbufDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gbufDesc, IID_PPV_ARGS(&mPSOs["gbuffer"])));


    // --- PSO: Lighting (back buffer, без depth теста) ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightDesc = gbufDesc;
    lightDesc.InputLayout = { nullptr, 0 }; // fullscreen triangle из VS без вершинного буфера
    lightDesc.pRootSignature = mRootSignature.Get();
    lightDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["lightingVS"]->GetBufferPointer()),
        mShaders["lightingVS"]->GetBufferSize()
    };
    lightDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["lightingPS"]->GetBufferPointer()),
        mShaders["lightingPS"]->GetBufferSize()
    };
    lightDesc.NumRenderTargets = 1;
    lightDesc.RTVFormats[0] = mBackBufferFormat;
    lightDesc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    lightDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    lightDesc.DepthStencilState.DepthEnable = FALSE;
    lightDesc.DepthStencilState.StencilEnable = FALSE;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&lightDesc, IID_PPV_ARGS(&mPSOs["lighting"])));

    // --- PSO: Wireframe ---

    // Key change: Rasterizer state with wireframe and NO culling
    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;  // Disable culling to see inside
    rasterDesc.FrontCounterClockwise = false;  // Adjust if your winding order is CCW
    rasterDesc.DepthBias = 0;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 0.0f;
    rasterDesc.DepthClipEnable = true;
    rasterDesc.MultisampleEnable = false;
    rasterDesc.AntialiasedLineEnable = true;
    rasterDesc.ForcedSampleCount = 0;
    rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframeDesc = {};
    wireframeDesc.InputLayout = { mWireframeInputLayout.data(), (UINT)mWireframeInputLayout.size() };
    wireframeDesc.pRootSignature = mRootSignature.Get();
    wireframeDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["wireframeVS"]->GetBufferPointer()),
        mShaders["wireframeVS"]->GetBufferSize()
    };
    wireframeDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["wireframePS"]->GetBufferPointer()),
        mShaders["wireframePS"]->GetBufferSize()
    };
    wireframeDesc.RasterizerState = rasterDesc;
    wireframeDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    wireframeDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    wireframeDesc.DepthStencilState.DepthEnable = TRUE; // depth test on to avoid visual artifacts
    wireframeDesc.DepthStencilState.StencilEnable = FALSE;
    wireframeDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // don't write depth
    wireframeDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    wireframeDesc.SampleMask = UINT_MAX;
    wireframeDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    wireframeDesc.NumRenderTargets = 1;
    wireframeDesc.RTVFormats[0] = mBackBufferFormat;  // рисуем в backbuffer
    wireframeDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    wireframeDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    wireframeDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wireframeDesc, IID_PPV_ARGS(&mPSOs["wireframe"])));
    
    // --- PSO: Sky ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyDesc = {};
    skyDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    skyDesc.pRootSignature = mRootSignature.Get();
    skyDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
        mShaders["skyVS"]->GetBufferSize()
    };
    skyDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
        mShaders["skyPS"]->GetBufferSize()
    };
    skyDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    skyDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    skyDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    skyDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    skyDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    skyDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    skyDesc.SampleMask = UINT_MAX;
    skyDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    skyDesc.NumRenderTargets = 1;
    skyDesc.RTVFormats[0] = mBackBufferFormat;
    skyDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    skyDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    skyDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyDesc, IID_PPV_ARGS(&mPSOs["sky"])));
}


// Создание ресурсов кадра: аллокатор команд и upload-буферы Pass/Material/Object/Terrain на каждый из gNumFrameResources кадров
void ShapesApp::BuildFrameResources()
{
    // ========================================================================
    // ВЫДЕЛЕНИЕ РЕСУРСОВ КАДРА ДЛЯ ТЕРРЕЙНА
    // ========================================================================
    // Terrain CB выделяется для каждого кадра отдельно
    // Размер = MAX_VISIBLE_NODES (256) - максимальное количество видимых узлов
    //
    // ВАЖНО: Terrain CB создаётся в FrameResource конструкторе,
    //        если terrainNodeCount > 0
    //
    // РАЗМЕРЫ БУФЕРОВ:
    //   - Pass CB: 1 запись (общий для всех объектов и террейна)
    //   - Material CB: количество материалов
    //   - Object CB: количество объектов (mAllRitems.size())
    //   - Terrain CB: MAX_VISIBLE_NODES (256) - по одной записи на видимый узел
    // ========================================================================
    UINT terrainNodeCount = 0;
    if (mTerrain)
    {
        // Максимальное количество видимых узлов террейна
        // Используется для выделения Terrain CB в каждом кадре
        terrainNodeCount = Terrain::TerrainSystem::GetMaxVisibleNodes();  // 256
    }
    
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        // Создание ресурсов кадра:
        // - Pass CB: 1 запись
        // - Object CB: по одной записи на каждый RenderItem
        // - Material CB: по одной записи на каждый материал
        // - Terrain CB: terrainNodeCount записей (для видимых узлов террейна)
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size(), (UINT)mMaterials.size(), terrainNodeCount));
    }
}

void ShapesApp::LoadTextures()
{
    auto negrAlbedo = std::make_unique<Texture>();
    negrAlbedo->Name = "negrAlbedo";
    negrAlbedo->Filename = L"Textures/african_head_diffuse.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        md3dDevice.Get(), mCommandList.Get(),
        negrAlbedo->Filename.c_str(),
        negrAlbedo->Resource, negrAlbedo->UploadHeap));

    mTextureVector.push_back({ negrAlbedo->Name, std::move(negrAlbedo) });
    //mTextures[negrAlbedo->Name] = std::move(negrAlbedo);

    auto negrNormal = std::make_unique<Texture>();
    negrNormal->Name = "negrNormal";
    negrNormal->Filename = L"Textures/african_head_nm.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        md3dDevice.Get(), mCommandList.Get(),
        negrNormal->Filename.c_str(),
        negrNormal->Resource, negrNormal->UploadHeap));

    mTextureVector.push_back({ negrNormal->Name, std::move(negrNormal) });
    //mTextures[negrNormal->Name] = std::move(negrNormal);

    auto bricks2 = std::make_unique<Texture>();
    bricks2->Name = "bricks2";
    bricks2->Filename = L"Textures/bricks2.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        md3dDevice.Get(), mCommandList.Get(),
        bricks2->Filename.c_str(),
        bricks2->Resource, bricks2->UploadHeap));

    mTextureVector.push_back({ bricks2->Name, std::move(bricks2) });
    //mTextures[bricks2->Name] = std::move(bricks2);

    auto bricks2Normal = std::make_unique<Texture>();
    bricks2Normal->Name = "bricks2Normal";
    bricks2Normal->Filename = L"Textures/bricks2_nmap.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        md3dDevice.Get(), mCommandList.Get(),
        bricks2Normal->Filename.c_str(),
        bricks2Normal->Resource, bricks2Normal->UploadHeap));

    mTextureVector.push_back({ bricks2Normal->Name, std::move(bricks2Normal) });
    //mTextures[bricks2Normal->Name] = std::move(bricks2Normal);

    //// Динамически для моделей
    //UINT texIndex = mTextureVector.size();  // Продолжаем нумерацию
    //for (const auto& model : mModels) {
    //    for (const auto& texPair : model.texturePaths) {
    //        if (!texPair.second.empty()) {
    //            auto tex = std::make_unique<Texture>();
    //            tex->Name = model.filename + "_" + texPair.first + "_diffuse";  // Уникальное имя
    //            tex->Filename = std::wstring(texPair.second.begin(), texPair.second.end()).c_str();
    //            // ... CreateDDSTextureFromFile12 (если DDS; иначе DirectX::CreateWICTextureFromFile12 для PNG/JPG)
    //            ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(), tex->Filename.c_str(), tex->Resource, tex->UploadHeap));
    //            mTextureVector.push_back({ tex->Name, std::move(tex) });
    //        }
    //    }
    //}
    //// Динамически для моделей: normal
    //for (const auto& model : mModels) {
    //    for (const auto& normalPair : model.normalTexturePaths) {
    //        if (!normalPair.second.empty()) {
    //            auto tex = std::make_unique<Texture>();
    //            tex->Name = model.filename + "_" + normalPair.first + "_normal";  // Уникальное имя
    //            tex->Filename = std::wstring(normalPair.second.begin(), normalPair.second.end());
    //            ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(), tex->Filename.c_str(), tex->Resource, tex->UploadHeap));
    //            mTextureVector.push_back({ tex->Name, std::move(tex) });
    //        }
    //    }
    //}

    // Динамически для моделей
    UINT texIndex = mTextureVector.size();  // Продолжаем нумерацию
    std::unordered_set<std::string> loadedDiffuseNames;  // Для проверки уникальности diffuse по имени (per material)
    std::unordered_set<std::string> loadedNormalNames;  // Для проверки уникальности normal по имени (per material)

    for (const auto& model : mModels) {
        for (size_t i = 0; i < model.texturePaths.size(); ++i) {
            const auto& texPair = model.texturePaths[i];
            std::string diffuseName = model.filename + "_" + texPair.first + "_diffuse";
            if (loadedDiffuseNames.find(diffuseName) == loadedDiffuseNames.end()) {
                if (!texPair.second.empty()) {  // Пропускаем, если путь пустой (хотя в LoadModel он заполняется default)
                    auto tex = std::make_unique<Texture>();
                    tex->Name = diffuseName;  // Уникальное имя
                    tex->Filename = std::wstring(texPair.second.begin(), texPair.second.end());
                    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(), tex->Filename.c_str(), tex->Resource, tex->UploadHeap));
                    mTextureVector.push_back({ tex->Name, std::move(tex) });
                    loadedDiffuseNames.insert(diffuseName);
                    std::cout << "Loaded unique diffuse: " << diffuseName << " from " << texPair.second << std::endl;
                }
                else {
                    std::cout << "Skipped empty diffuse for " << diffuseName << " (using default, but not loading duplicate)" << std::endl;
                }
            }
            else {
                std::cout << "Skipped duplicate diffuse for " << diffuseName << std::endl;
            }

            const auto& normalPair = model.normalTexturePaths[i];
            std::string normalName = model.filename + "_" + normalPair.first + "_normal";
            if (loadedNormalNames.find(normalName) == loadedNormalNames.end()) {
                if (!normalPair.second.empty()) {
                    auto tex = std::make_unique<Texture>();
                    tex->Name = normalName;  // Уникальное имя
                    tex->Filename = std::wstring(normalPair.second.begin(), normalPair.second.end());
                    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(), tex->Filename.c_str(), tex->Resource, tex->UploadHeap));
                    mTextureVector.push_back({ tex->Name, std::move(tex) });
                    loadedNormalNames.insert(normalName);
                    std::cout << "Loaded unique normal: " << normalName << " from " << normalPair.second << std::endl;
                }
                else {
                    std::cout << "Skipped empty normal for " << normalName << " (using default, but not loading duplicate)" << std::endl;
                }
            }
            else {
                std::cout << "Skipped duplicate normal for " << normalName << std::endl;
            }
        }
    }

}

void ShapesApp::BuildMaterials()
{
    UINT matCBIndex = 0;
    UINT k = 0;

    auto negr = std::make_unique<Material>();
    negr->Name = "negr";
    negr->MatCBIndex = matCBIndex++;
    negr->DiffuseSrvHeapIndex = k++;
    negr->NormalSrvHeapIndex = k++;
    negr->DiffuseAlbedo = XMFLOAT4(0.1f, 0.1f, 0.1f, 1.0f);
    negr->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    negr->Roughness = 0.9f;

    auto brick2 = std::make_unique<Material>();
    brick2->Name = "brick2";
    brick2->MatCBIndex = matCBIndex++;
    brick2->DiffuseSrvHeapIndex = k++;
    brick2->NormalSrvHeapIndex = k++;
    brick2->DiffuseAlbedo = XMFLOAT4(0.4f, 0.4f, 0.4f, 1.0f);
    brick2->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    brick2->Roughness = 0.7f;

    mMaterials["negr"] = std::move(negr);
    mMaterials["brick2"] = std::move(brick2);

    // Динамически для моделей
    for (const auto& model : mModels) {
        for (size_t i = 0; i < model.materialNames.size(); ++i) {
            std::string matName = model.filename + "_" + model.materialNames[i];
            if (mMaterials.find(matName) == mMaterials.end()) {
                auto mat = std::make_unique<Material>();
                mat->Name = matName;
                mat->MatCBIndex = matCBIndex++;

                mat->DiffuseAlbedo = model.diffuseColors[i];  // Kd для Materials
                //std::cout << "Using solid color for " << matName << ": (" << mat->DiffuseAlbedo.x << "," << mat->DiffuseAlbedo.y << "," << mat->DiffuseAlbedo.z << ")" << std::endl;

                mat->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
                mat->Roughness = 0.5f;
                mat->DiffuseSrvHeapIndex = k++;  // Из mTextureVector (по имени)
                mat->NormalSrvHeapIndex = k++;   // normal
                std::cout << "Created mat " << matName << ": diffuseIdx=" << mat->DiffuseSrvHeapIndex << ", normalIdx=" << mat->NormalSrvHeapIndex << std::endl;
                mMaterials[matName] = std::move(mat);
            }
        }
    }
}

void ShapesApp::BuildRenderItems() {
    auto geo = mGeometries["shapeGeo"].get();

    UINT objCBIndex = 0;
    size_t globalSubmeshIndex = 0;  // Глобальный индекс в geo->DrawArgs

    // Для каждой модели
    for (size_t modelIdx = 0; modelIdx < mModels.size(); ++modelIdx)
    {
        const auto& model = mModels[modelIdx];
        // Пропускаем LOD-модели при создании RenderItem — они используются только для заполнения LOD-уровней
        if (model.filename.find("_lod01") != std::string::npos || model.filename.find("_lod02") != std::string::npos)
        {
            // Продвигаем глобальный индекс, так как геометрия LOD присутствует в shapeGeo
            globalSubmeshIndex += model.meshes.size();
            continue;
        }
        for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
        {
            // Проверяем, что globalSubmeshIndex < geo->DrawArgs.size()
            if (globalSubmeshIndex >= geo->DrawArgs.size())
                break;

            auto& drawArg = geo->DrawArgs.at("mesh" + std::to_string(globalSubmeshIndex));

            auto renderItem = std::make_unique<RenderItem>();
            renderItem->Name = model.filename + "_mesh" + std::to_string(meshIdx);  // Уникальное имя
            renderItem->Visible = true;
            renderItem->Pos = XMFLOAT3(0.0f, 0.0f, 0.0f);
            renderItem->Rot = XMFLOAT3(0.0f, 0.0f, 0.0f);
            renderItem->Scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            renderItem->UpdateWorld();
            XMStoreFloat4x4(&renderItem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
            renderItem->ObjCBIndex = objCBIndex++;
            renderItem->Geo = geo;
            renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            renderItem->IndexCount = drawArg.IndexCount;
            renderItem->StartIndexLocation = drawArg.StartIndexLocation;
            renderItem->BaseVertexLocation = drawArg.BaseVertexLocation;
            renderItem->BoundsLocal = drawArg.Bounds; // сохраняем local-space bounds для куллинга

            // init lods vector
            LODEntry lod0;
            lod0.Geo = geo;
            lod0.IndexCount = drawArg.IndexCount;
            lod0.StartIndexLocation = drawArg.StartIndexLocation;
            lod0.BaseVertexLocation = drawArg.BaseVertexLocation;
            lod0.BoundsLocal = drawArg.Bounds;
            renderItem->LODs.push_back(lod0);

            // Материал из этой модели
            Material* assignedMat = nullptr;
            if (meshIdx < model.materialNames.size()) {
                std::string matName = model.filename + "_" + model.materialNames[meshIdx];  // Уникальное: "african_head_mat1"
                auto it = mMaterials.find(matName);
                if (it != mMaterials.end()) {
                    assignedMat = it->second.get();
                }
            }
            renderItem->Mat = assignedMat ? assignedMat : mMaterials["negr"].get();  // материал заглушка

            // Try to attach LOD1/LOD2 if present in mModels with matching stem
            std::string stem = GetModelStem(model.filename); // african_head
            // Find indices of any _lod01/_lod02 entries with same stem
            size_t lod1ModelIdx = SIZE_MAX;
            size_t lod2ModelIdx = SIZE_MAX;
            for (size_t i = 0; i < mModels.size(); ++i)
            {
                std::string st = GetModelStem(mModels[i].filename); // african_head
                if (st == stem)
                {
                    if (mModels[i].filename.find("_lod01") != std::string::npos) // елил есть _lod01
                        lod1ModelIdx = i; // индекс модели с _lod01
                    else if (mModels[i].filename.find("_lod02") != std::string::npos) // елил есть _lod02
                        lod2ModelIdx = i; // индекс модели с _lod02
                }
            }

            auto addLodFromModel = [&](size_t mdlIdx)
                {
                    if (mdlIdx == SIZE_MAX) return;
                    const auto& lodModel = mModels[mdlIdx];
                    
                    UINT offsetStart = 0; 
                    for (size_t i = 0; i < mdlIdx; ++i)
                        offsetStart += (UINT)mModels[i].meshes.size(); // кол-во мешей в моделях до нашего меша lod1/2

                    // Перепроверка индекса
                    size_t lodMeshIdx = meshIdx < lodModel.meshes.size() ? meshIdx : (lodModel.meshes.empty() ? 0 : lodModel.meshes.size() - 1); // индекс меша в нужной LOD модели
                    size_t subIdx = offsetStart + lodMeshIdx; // высчитываем индекс меша с лодом среди вообще всех мешей
                    std::string key = "mesh" + std::to_string(subIdx);
                    auto itDraw = mGeometries["shapeGeo"]->DrawArgs.find(key); // если нашли лод
                    if (itDraw != mGeometries["shapeGeo"]->DrawArgs.end()) // то добавляем его к LOD0
                    {
                        LODEntry lod;
                        lod.Geo = mGeometries["shapeGeo"].get();
                        lod.IndexCount = itDraw->second.IndexCount;
                        lod.StartIndexLocation = itDraw->second.StartIndexLocation;
                        lod.BaseVertexLocation = itDraw->second.BaseVertexLocation;
                        lod.BoundsLocal = itDraw->second.Bounds;
                        renderItem->LODs.push_back(lod);
                    }
                };

            // LOD0 уже добавлен
            // Добавляем LOD1 и LOD2 при наличии
            addLodFromModel(lod1ModelIdx);
            addLodFromModel(lod2ModelIdx);

            mAllRitems.push_back(std::move(renderItem));
            mOpaqueRitems.push_back(mAllRitems.back().get());  // Все в opaque (кроме lights)

            globalSubmeshIndex++;

            std::cout << "RenderItem for model '" << model.filename << "' submesh " << meshIdx
                << " (global index: " << globalSubmeshIndex
                << ", submesh name: '" << model.submeshNames[meshIdx] << "') "
                << "assigned material '" << (assignedMat ? assignedMat->Name : "fallback_negr") << "' "
                << "with diffuse SRV index " << (assignedMat ? assignedMat->DiffuseSrvHeapIndex : -1) << " and with normal SRV index " << (assignedMat ? assignedMat->NormalSrvHeapIndex : -1) << std::endl;
        }
    }

	// --- Spawn a 100x100 grid of model instances for FPS testing ---
	if (!mOpaqueRitems.empty())
	{
		RenderItem* prototype = mOpaqueRitems[0];
		const int gridX = 10;
		const int gridY = 10;
		const float spacing = 2.0f; // distance between instances
		for (int gy = 0; gy < gridY; ++gy)
		{
			for (int gx = 0; gx < gridX; ++gx)
			{
				// skip origin cell to avoid duplicating prototype at (0,0)
				if (gx == 0 && gy == 0) continue;
				auto inst = std::make_unique<RenderItem>(*prototype);
				inst->ObjCBIndex = objCBIndex++;
				inst->Pos = XMFLOAT3(
					(gx - gridX / 2) * spacing,
					prototype->Pos.y,
					(gy - gridY / 2) * spacing);
				inst->UpdateWorld();
				inst->Visible = true;
				// world bounds will be updated later in culling pass
				mAllRitems.push_back(std::move(inst));
				mOpaqueRitems.push_back(mAllRitems.back().get());
			}
		}
	}

	// --- Sky Render Item ---
	auto skyGeo = mGeometries["skyGeo"].get();
	auto skyRitem = std::make_unique<RenderItem>();
	skyRitem->Name = "sky";
	skyRitem->Visible = true;
	skyRitem->Pos = XMFLOAT3(0.0f, 0.0f, 0.0f);
	skyRitem->Rot = XMFLOAT3(0.0f, 0.0f, 0.0f);
	skyRitem->Scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
	skyRitem->UpdateWorld();
	skyRitem->ObjCBIndex = objCBIndex++;
	skyRitem->Geo = skyGeo;
	skyRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	skyRitem->IndexCount = skyGeo->DrawArgs["sky"].IndexCount;
	skyRitem->StartIndexLocation = skyGeo->DrawArgs["sky"].StartIndexLocation;
	skyRitem->BaseVertexLocation = skyGeo->DrawArgs["sky"].BaseVertexLocation;
	
	mSkyRitem = skyRitem.get();
	mAllRitems.push_back(std::move(skyRitem));

	InitLights(objCBIndex);
}

// Отрисовка списка элементов: установка VB/IB, для каждого элемента — привязка CB (object, material), DrawIndexedInstanced
void ShapesApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();

    // For each render item...
    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        if (!ri->Visible)
            continue;

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        assert(ri->Mat != nullptr);
        assert(ri->Mat->MatCBIndex < mMaterials.size());

        auto mat = mMaterials[ri->Mat->Name].get();
        CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
        texHandle.Offset(mat->DiffuseSrvHeapIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(4, texHandle); // t0 - Изменено с 3 на 4

        texHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
        texHandle.Offset(mat->NormalSrvHeapIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(5, texHandle); // t1 - Изменено с 4 на 5

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(1, matCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);

    }
}

// Отрисовка элементов в режиме каркаса (wireframe PSO, тот же список элементов)
void ShapesApp::DrawWireframeRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();

    // For each render item...
    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];
        if (!ri->Visible)
            continue;

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(1, matCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);

    }
}

void ShapesApp::InitLights(UINT objCBIndex)
{
    int k = 0;
    mOldestActiveShootLight = 0;

    // directional lights
    mMainPassCB.Lights[k].Direction = { 0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[k].Strength = { 0.3f, 0.3f, 0.3f };
    mMainPassCB.Lights[k].Type = 0;
    k++;

    mMainPassCB.Lights[k].Direction = { -0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[k].Strength = { 0.3f, 0.3f, 0.3f };
    mMainPassCB.Lights[k].Type = 0;
    k++;

    mMainPassCB.Lights[k].Direction = { 0.0f, -0.707f, -0.707f };
    mMainPassCB.Lights[k].Strength = { 0.3f, 0.3f, 0.3f };
    mMainPassCB.Lights[k].Type = 0;
    k++;

    // spot lights
    mMainPassCB.Lights[k].Position = { -2.0f, 2.0f, 2.0f };
    mMainPassCB.Lights[k].Strength = { 10.0f, 1.0f, 1.0f };
    mMainPassCB.Lights[k].Direction = { 1.0f, -1.0f, -1.0f };
    mMainPassCB.Lights[k].FalloffStart = 0.5f;
    mMainPassCB.Lights[k].FalloffEnd = 10.0f;
    mMainPassCB.Lights[k].SpotPower = 0.1f;
    mMainPassCB.Lights[k].Type = 2;
    k++;

    // point lights
    mMainPassCB.Lights[k].Position = { 2.0f, 1.0f, 2.0f };
    mMainPassCB.Lights[k].Strength = { 1.0f, 1.0f, 10.0f };
    mMainPassCB.Lights[k].FalloffStart = 0.5f;
    mMainPassCB.Lights[k].FalloffEnd = 10.0f;
    mMainPassCB.Lights[k].Type = 1;
    k++;

    lightsCount = k;

    auto lightGeo = mGeometries["lightGeo"].get();

    // Создаём RenderItems для дебага
    for (int i = 0; i < lightsCount; ++i)
    {
        auto& light = mMainPassCB.Lights[i];
        if (light.Type == 1) // point
        {
            auto renderItem = std::make_unique<LightningRenderItem>();
            renderItem->LightIndex = i;
            renderItem->Visible = false;
            renderItem->isDebug = true;
            renderItem->lightObject = &light; // привязка света к LightningRenderItem
            /// стартовые значения предметов
            renderItem->Pos = light.Position;
            renderItem->Rot = XMFLOAT3(0.0f, 0.0f, 0.0f);
            renderItem->Scale = XMFLOAT3((std::max)(0.1f, light.FalloffEnd), (std::max)(0.1f, light.FalloffEnd), (std::max)(0.1f, light.FalloffEnd));
            renderItem->UpdateWorld(); /// world ///
            XMStoreFloat4x4(&renderItem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
            renderItem->ObjCBIndex = objCBIndex++;
            renderItem->Mat = mMaterials["negr"].get();
            renderItem->Geo = lightGeo;
            renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            renderItem->IndexCount = lightGeo->DrawArgs["mesh0"].IndexCount;
            renderItem->StartIndexLocation = lightGeo->DrawArgs["mesh0"].StartIndexLocation;
            renderItem->BaseVertexLocation = lightGeo->DrawArgs["mesh0"].BaseVertexLocation;

            mAllRitems.push_back(std::move(renderItem)); // TODO 
            mWireframeRitems.push_back(mAllRitems.back().get()); // TODO 
        }
        else if (light.Type == 2) // spot
        {
            auto renderItem = std::make_unique<LightningRenderItem>();
            renderItem->LightIndex = i;
            renderItem->Visible = false;
            renderItem->isDebug = true;
            renderItem->lightObject = &light; // привязка света к LightningRenderItem
            /// стартовые значения предметов
            renderItem->Pos = light.Position;
            renderItem->Rot = XMFLOAT3(0.0f, 0.0f, 0.0f);
            renderItem->Scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            renderItem->UpdateWorld(); /// world ///
            XMStoreFloat4x4(&renderItem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
            renderItem->ObjCBIndex = objCBIndex++;
            renderItem->Mat = mMaterials["negr"].get();
            renderItem->Geo = lightGeo;
            renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            renderItem->IndexCount = lightGeo->DrawArgs["mesh1"].IndexCount;
            renderItem->StartIndexLocation = lightGeo->DrawArgs["mesh1"].StartIndexLocation;
            renderItem->BaseVertexLocation = lightGeo->DrawArgs["mesh1"].BaseVertexLocation;

            mAllRitems.push_back(std::move(renderItem)); // TODO 
            mWireframeRitems.push_back(mAllRitems.back().get()); // TODO 
        }
    }

    // ... существующие источники (5 штук) ...
    MAX_SHOOT_LIGHTS = MaxLights - lightsCount;
    // Добавляем 11 point lights для стрельбы (изначально неактивные)
    for (int i = k; i < k + MAX_SHOOT_LIGHTS; ++i)
    {
        mMainPassCB.Lights[i].Position = { 0.0f, 0.0f, 0.0f };
        mMainPassCB.Lights[i].Strength = { 0.0f, 0.0f, 0.0f };
        mMainPassCB.Lights[i].FalloffStart = 2.0f;
        mMainPassCB.Lights[i].FalloffEnd = 20.0f;
        mMainPassCB.Lights[i].Type = 1;

        // Создаем RenderItem для этого источника
        auto renderItem = std::make_unique<LightningRenderItem>();
        renderItem->LightIndex = i;
        renderItem->Visible = false;
        renderItem->isDebug = true;
        renderItem->lightObject = &mMainPassCB.Lights[i];
        renderItem->Pos = { 0.0f, 0.0f, 0.0f };
        renderItem->Rot = XMFLOAT3(0.0f, 0.0f, 0.0f);
        renderItem->Scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
        renderItem->UpdateWorld();
        renderItem->ObjCBIndex = objCBIndex++;
        renderItem->Mat = mMaterials["negr"].get();
        renderItem->Geo = lightGeo;
        renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        renderItem->IndexCount = lightGeo->DrawArgs["mesh0"].IndexCount;
        renderItem->StartIndexLocation = lightGeo->DrawArgs["mesh0"].StartIndexLocation;
        renderItem->BaseVertexLocation = lightGeo->DrawArgs["mesh0"].BaseVertexLocation;
        renderItem->Visible = false;

        mAllRitems.push_back(std::move(renderItem));
        mShootWireframeRitems.push_back(mAllRitems.back().get());
        mShootLightVelocities.push_back({ 0.0f, 0.0f, 0.0f });
        mShootLightActive.push_back(false);
    }
}

// Создание G-Buffer: текстуры альбедо и нормалей (RTV + SRV), размер под текущее окно
void ShapesApp::BuildGBuffer()
{
    // Переразмещаем RTV дескрипторы для G-Buffer ПОВЕРХ текущего RTV heap (который унаследован от D3DApp).
    // В D3DApp RTV heap обычно создан на SwapChainBufferCount; нужно ещё +2 для G-Buffer.
    // Допустимо размещать "поверх", если ты при создании RTV heap заранее заложишь нужное число дескрипторов.
    // Ниже предполагается, что mRtvHeap вмещает SwapChainBufferCount + 2.

    // Создаём RTV heap только для G-Buffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 2; // Albedo + Normal (можно 3, если хочешь Position/Depth)
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mGBufferRtvHeap)));

    mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Создаём ресурсы
    D3D12_RESOURCE_DESC rtDesc = {};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Alignment = 0;
    rtDesc.Width = mClientWidth;
    rtDesc.Height = mClientHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.SampleDesc.Quality = 0;
    rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Albedo
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CD3DX12_CLEAR_VALUE clearAlbedo(mBackBufferFormat, Colors::Black);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearAlbedo,
        IID_PPV_ARGS(&mGBuffer.Albedo)));

    // Normal
    rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    CD3DX12_CLEAR_VALUE clearNormal(DXGI_FORMAT_R16G16B16A16_FLOAT, Colors::Black);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &rtDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearNormal,
        IID_PPV_ARGS(&mGBuffer.Normal)));



    // RTV размещаем после backbuffer RTV-ов
    auto rtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mGBufferRtvHeap->GetCPUDescriptorHandleForHeapStart());

    mGBuffer.AlbedoRTV = rtvHandle;
    md3dDevice->CreateRenderTargetView(mGBuffer.Albedo.Get(), nullptr, mGBuffer.AlbedoRTV);

    rtvHandle.Offset(1, mRtvDescriptorSize);
    mGBuffer.NormalRTV = rtvHandle;
    md3dDevice->CreateRenderTargetView(mGBuffer.Normal.Get(), nullptr, mGBuffer.NormalRTV);

    // --- Depth (типлесный) ---
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Alignment = 0;
    depthDesc.Width = mClientWidth;
    depthDesc.Height = mClientHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT; // <--- ключ // DXGI_FORMAT_R32_TYPELESS
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = DXGI_FORMAT_D32_FLOAT; // для DSV
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear,
        IID_PPV_ARGS(&mDepthStencilBuffer)
    ));

    // --- DSV ---
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT; // D32_FLOAT
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    md3dDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), &dsvDesc, DepthStencilView());
}


// создаем srv heap текстур, в которых мы задаем параметры дескрипторам и привязываем материалам нужные индексы 
//
void ShapesApp::BuildSrvHeap()
{
    // Сколько SRV нам нужно:
    //  - у нас 2 для G-Buffer (Albedo, Normal)
    //  - N для текстур материалов (по факту 2 штуки в LoadTextures)
    const UINT materialSrvCount = mTextureVector.size(); // diffuse + normal для одного материала как минимум
    const UINT gbufferSrvCount = 3;
    const UINT totalSrv = materialSrvCount + gbufferSrvCount;

    // создали descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = totalSrv;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap)));

    const UINT srvInc = mCbvSrvDescriptorSize;

    // текстуры (начало heap)
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuStart(mSrvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuStart(mSrvHeap->GetGPUDescriptorHandleForHeapStart());

    /*for (auto& texPair : mTextures)
    {
        auto tex = texPair.second.get();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = tex->Resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, cpuStart);
        cpuStart.Offset(1, srvInc);
    }*/

    for (auto& texPair : mTextureVector)
    {
        auto tex = texPair.second.get();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = tex->Resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, cpuStart);
        cpuStart.Offset(1, srvInc);
    }

    // ---- G-Buffer SRV (идут следом) ----
    {
        mGBuffer.SrvOffset = materialSrvCount;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;

        // Albedo SRV at index 2
        auto desc = mGBuffer.Albedo->GetDesc();
        srv.Format = mGBuffer.AlbedoFormat;
        srv.Texture2D.MipLevels = desc.MipLevels;
        md3dDevice->CreateShaderResourceView(mGBuffer.Albedo.Get(), &srv, cpuStart);
        cpuStart.Offset(1, srvInc);

        // Normal SRV at index 3
        desc = mGBuffer.Normal->GetDesc();
        srv.Format = mGBuffer.NormalFormat;
        srv.Texture2D.MipLevels = desc.MipLevels;
        md3dDevice->CreateShaderResourceView(mGBuffer.Normal.Get(), &srv, cpuStart);
        cpuStart.Offset(1, srvInc);

        // Depth SRV at index 4
        D3D12_SHADER_RESOURCE_VIEW_DESC srvD = {};
        srvD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvD.Format = DXGI_FORMAT_R32_FLOAT; // R32_FLOAT
        srvD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvD.Texture2D.MostDetailedMip = 0;
        srvD.Texture2D.MipLevels = 1;
        srvD.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &srvD, cpuStart);

        // GPU handle таблицы (указывает на начало диапазона G-Buffer SRV)
        auto gpu = gpuStart;
        gpu.Offset(mGBuffer.SrvOffset, srvInc);
        mGBuffer.TableGPU = gpu;
    }
    std::cout << "SRV Heap populated: Total " << mTextureVector.size() << " textures." << std::endl;
    for (size_t i = 0; i < mTextureVector.size(); ++i) {
        std::string filename = "";
        for (int kl = 0; kl < mTextureVector[i].second->Filename.size(); kl++)
        {
            filename.push_back(mTextureVector[i].second->Filename[kl]);
        }

        std::cout << "Index " << i << ": " << mTextureVector[i].first << " (" << filename << ")" << std::endl;
    }
}

void ShapesApp::UpdateGBufferSrvs()
{
    if (mSrvHeap == nullptr) return;

    UINT srvOffset = mGBuffer.SrvOffset;
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        srvOffset,
        mCbvSrvDescriptorSize);

    // Albedo SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    srvDesc.Format = mGBuffer.AlbedoFormat;
    md3dDevice->CreateShaderResourceView(mGBuffer.Albedo.Get(), &srvDesc, cpuHandle);
    cpuHandle.Offset(1, mCbvSrvDescriptorSize);

    // Normal SRV
    srvDesc.Format = mGBuffer.NormalFormat;
    md3dDevice->CreateShaderResourceView(mGBuffer.Normal.Get(), &srvDesc, cpuHandle);
    cpuHandle.Offset(1, mCbvSrvDescriptorSize);

    // Depth SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MostDetailedMip = 0;
    depthSrvDesc.Texture2D.MipLevels = 1;
    depthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &depthSrvDesc, cpuHandle);
}


std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> ShapesApp::GetStaticSamplers()
{
    // Applications usually only need a handful of samplers.  So just define them all up front
    // and keep them available as part of the root signature.  

    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
        0.0f,                             // mipLODBias
        8);                               // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
        0.0f,                              // mipLODBias
        8);                                // maxAnisotropy

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp };
}


void ShapesApp::ShootLight()
{
    // Ищем неактивный источник
    int availableIndex = -1;
    for (int i = 0; i < MAX_SHOOT_LIGHTS; ++i)
    {
        if (!mShootLightActive[i])
        {
            availableIndex = i;
            break;
        }
    }

    // Если все источники активны, используем самый старый
    if (availableIndex == -1)
    {
        availableIndex = mOldestActiveShootLight;
        mOldestActiveShootLight = (mOldestActiveShootLight + 1) % MAX_SHOOT_LIGHTS;
    }

    // Активируем источник
    mShootLightActive[availableIndex] = true;

    // Устанавливаем позицию источника в позицию камеры
    mShootWireframeRitems[availableIndex]->Pos = mCamera.GetPosition3f();

    // Устанавливаем случайный цвет
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<float> dis(0.5f, 1.0f);

    XMFLOAT3 randomColor = { dis(gen) * 2, dis(gen) * 2, dis(gen) * 2 };
    mMainPassCB.Lights[5 + availableIndex].Strength = randomColor;
    
    // DEBUG: Log new shoot light
    if (DebugFlags::LightingSystem)
    {
        std::cout << "[LIGHTS] Shoot Light " << availableIndex << " SPAWNED at (" 
                  << mShootWireframeRitems[availableIndex]->Pos.x << ", " 
                  << mShootWireframeRitems[availableIndex]->Pos.y << ", " 
                  << mShootWireframeRitems[availableIndex]->Pos.z << ") Color: (" 
                  << randomColor.x << ", " << randomColor.y << ", " << randomColor.z << ")\n";
    }

    // Устанавливаем направление движения (вперед от камеры)
    XMVECTOR forward = mCamera.GetLook();
    XMStoreFloat3(&mShootLightVelocities[availableIndex], forward * 20.0f);

    // Обновляем мировую матрицу
    mShootWireframeRitems[availableIndex]->UpdateWorld();

    // Делаем источник видимым
    mShootWireframeRitems[availableIndex]->Visible = true;
}


// Инициализация атмосферы: создание ресурсов, дескрипторов
void ShapesApp::BuildAtmosphere()
{
    mAtmosphere = std::make_unique<Atmosphere>(md3dDevice.Get(), mClientWidth, mClientHeight, 
                                                DXGI_FORMAT_R16G16B16A16_FLOAT);
    
    // Установка параметров по умолчанию (чистая атмосфера)
    mAtmosphere->SetCleanAtmosphere();
}

// Обновление константного буфера атмосферы каждый кадр
void ShapesApp::UpdateAtmosphereCB(const GameTimer& gt)
{
    if (!mAtmosphere || !mCurrFrameResource->AtmosphereCB)
        return;
    
    auto& params = mAtmosphere->GetParameters();
    
    AtmosphereConstants atmoCB;
    atmoCB.SunDirection = params.SunDirection;
    atmoCB.SunIntensity = params.SunIntensity;
    atmoCB.RayleighScattering = params.RayleighCoefficients;
    atmoCB.PlanetRadius = params.PlanetRadius * 1000.0f; // km to meters
    atmoCB.MieScattering = params.MieCoefficients;
    atmoCB.AtmosphereRadius = (params.PlanetRadius + params.AtmosphereHeight) * 1000.0f; // km to meters
    atmoCB.RayleighScaleHeight = params.RayleighScaleHeight;
    atmoCB.MieScaleHeight = params.MieScaleHeight;
    atmoCB.MieAnisotropy = params.MieAnisotropy;
    atmoCB.AtmosphereDensity = params.DensityMultiplier;
    
    // Позиция камеры в километрах
    XMFLOAT3 camPos;
    XMStoreFloat3(&camPos, mCamera.GetPosition());
    atmoCB.CameraPositionKm = XMFLOAT3(camPos.x / 1000.0f, camPos.y / 1000.0f, camPos.z / 1000.0f);
    
    atmoCB.Exposure = params.Exposure;
    atmoCB.NumSamples = params.NumViewSamples;
    atmoCB.NumLightSamples = params.NumLightSamples;
    atmoCB.EnableAtmosphere = mEnableAtmosphere ? 1 : 0; // Pass enable flag to shader
    
    mCurrFrameResource->AtmosphereCB->CopyData(0, atmoCB);
}

// Отрисовка неба с атмосферным рассеянием
void ShapesApp::DrawSky(ID3D12GraphicsCommandList* cmdList)
{
    if (!mEnableAtmosphere || !mSkyRitem)
        return;
    
    // Установка PSO для неба
    cmdList->SetPipelineState(mPSOs["sky"].Get());
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());
    
    // Установка дескрипторных куч
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    
    // Установка render target и depth stencil
    auto backRtv = CurrentBackBufferView();
    auto dsv = DepthStencilView();
    cmdList->OMSetRenderTargets(1, &backRtv, FALSE, &dsv);
    
    // Привязка константных буферов
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    auto objCBAddress = mCurrFrameResource->ObjectCB->Resource()->GetGPUVirtualAddress();
    objCBAddress += mSkyRitem->ObjCBIndex * objCBByteSize;
    cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
    
    cmdList->SetGraphicsRootConstantBufferView(2, mCurrFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(3, mCurrFrameResource->AtmosphereCB->Resource()->GetGPUVirtualAddress()); // Изменено с 2 на 3
    
    // Отрисовка неба
    cmdList->IASetVertexBuffers(0, 1, &mSkyRitem->Geo->VertexBufferView());
    cmdList->IASetIndexBuffer(&mSkyRitem->Geo->IndexBufferView());
    cmdList->IASetPrimitiveTopology(mSkyRitem->PrimitiveType);
    
    cmdList->DrawIndexedInstanced(mSkyRitem->IndexCount, 1, 
                                   mSkyRitem->StartIndexLocation, 
                                   mSkyRitem->BaseVertexLocation, 0);
}
