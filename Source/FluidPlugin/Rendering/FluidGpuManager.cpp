#include <FluidPlugin/FluidPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/IdTable.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Utilities/Stats.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Profiling/Profiling.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>

#include <Shaders/Fluid/FluidSolverConstants.h>

plCVarBool cvar_FluidEnable("Fluid.Enable", true, plCVarFlags::Default, "Steps fluid volumes each frame; off leaves every consumer sampling nothing");
plCVarInt cvar_FluidMaxActiveVolumes("Fluid.MaxActiveVolumes", 8, plCVarFlags::Default, "Most volumes simulated per world per frame, highest priority then nearest camera first. The rest freeze");
plCVarFloat cvar_FluidResolutionScale("Fluid.ResolutionScale", 1.0f, plCVarFlags::Default, "Scales every volume's grid resolution. The quality tier lever");
plCVarFloat cvar_FluidPressureIterationScale("Fluid.PressureIterationScale", 1.0f, plCVarFlags::Default, "Scales the pressure iterations of every Gas volume");
plCVarFloat cvar_FluidMaxStepTime("Fluid.MaxStepTime", 1.0f / 30.0f, plCVarFlags::Default, "Longest time one solver step may simulate. A longer frame simulates in slow motion rather than exploding");
plCVarInt cvar_FluidAtlasMaxSize("Fluid.AtlasMaxSize", 2048, plCVarFlags::Default, "Largest edge of a world's fluid output atlas. Volumes that no longer fit are frozen");
plCVarBool cvar_FluidSmokeVolumes("Fluid.SmokeVolumes", true, plCVarFlags::Default, "Extrudes Gas volumes with a Fluid Smoke Render component into 3D smoke the VDB volume renderer draws. A global kill switch: the component is the per-volume opt-in");
plCVarInt cvar_FluidDebugMaterial("Fluid.Debug.Material", 0, plCVarFlags::Default, "Debug view of fluid materials: 0 lit, 1 clay, 2 normal, 3 height, 4 foam, 5 detail frame, 6 mirror, 7 specular, 8 body");
plCVarInt cvar_FluidDebugMaterialLayers("Fluid.Debug.MaterialLayers", 0, plCVarFlags::Default, "Bit mask of fluid material layers to switch off. FluidWater: 1 ripples, 2 swell, 4 absorption and refraction, 8 caustics, 16 foam, 32 translucency, 64 simulated normal");

namespace
{
  constexpr plUInt32 FluidMinResolution = 64;
  constexpr plUInt32 FluidMaxResolution = 1024;
  constexpr plUInt32 FluidMinAtlasSize = 256;

  /// Seconds for the snow disturbance signal to halve. Short enough that the spray stops soon after a footstep, long
  /// enough that the activity readback, which samples only every few frames, still sees a single footstep.
  constexpr float FluidDisturbanceHalfLife = 0.15f;

  /// Water steps at least this long after its last source, whatever the volume's Sleep After, so waves can settle.
  constexpr float FluidWaterMinSleepAfter = 15.0f;

  /// Water: the slowest flow the sub-step count is planned for, m/s. Sources that ask for faster flow raise it.
  constexpr float FluidWaterMinPlannedFlow = 2.0f;
  constexpr plUInt32 FluidMaxSmokeResolution = 128;
  constexpr plUInt32 FluidActivityCells = plFluidGpuManager::SnowActivityGridSize * plFluidGpuManager::SnowActivityGridSize;
  constexpr plUInt64 FluidActivityInterval = 4; ///< frames between two activity readbacks of one volume

  plUInt32 RoundUpPowerOfTwo(plUInt32 uiValue)
  {
    plUInt32 uiResult = 1;
    while (uiResult < uiValue)
      uiResult <<= 1;
    return uiResult;
  }

  plUInt32 ThreadGroups(plUInt32 uiResolution)
  {
    return (uiResolution + FLUID_SOLVER_THREADS - 1) / FLUID_SOLVER_THREADS;
  }

  /// Bins the sources into FLUID_SOURCE_TILES^2 tiles. out_ranges receives (first, count) per tile, row by row, into
  /// out_indices. A source's reach has two texels of margin, so kernels may evaluate it on the faces of their texel. A
  /// source whose tiles would overflow FLUID_MAX_SOURCE_INDICES is left out entirely; returns false when that happened.
  bool BinSources(plArrayPtr<const plFluidSourceGpu> sources, plUInt32 uiRes, plArrayPtr<plVec2U32> out_ranges, plDynamicArray<plUInt32>& out_indices)
  {
    constexpr plUInt32 uiTiles = FLUID_SOURCE_TILES;
    const float fTileSize = plMath::Max(static_cast<float>(uiRes) / uiTiles, 1.0f);
    const float fRes = static_cast<float>(uiRes);

    struct TileRect
    {
      plUInt32 m_uiX0 = 0, m_uiY0 = 0, m_uiX1 = 0, m_uiY1 = 0;
      bool m_bUsed = false;
    };

    plHybridArray<TileRect, 64> rects;
    plUInt32 counts[uiTiles * uiTiles] = {};
    plUInt32 uiTotal = 0;
    bool bAllFit = true;

    for (const plFluidSourceGpu& src : sources)
    {
      TileRect& rect = rects.ExpandAndGetRef();

      const float fRadius = plMath::Max(src.Radius, 0.5f);
      float fReach = static_cast<plUInt32>(src.Shape) == FLUID_SOURCE_VORTEX ? 4.0f * plMath::Max(fRadius, 1.0f) : fRadius + src.Extents.GetLength() + src.Axis.GetLength();
      fReach += 2.0f;

      const float fMinX = plMath::Min(src.Position.x, src.PrevPosition.x) - fReach;
      const float fMinY = plMath::Min(src.Position.y, src.PrevPosition.y) - fReach;
      const float fMaxX = plMath::Max(src.Position.x, src.PrevPosition.x) + fReach;
      const float fMaxY = plMath::Max(src.Position.y, src.PrevPosition.y) + fReach;

      if (fMaxX < 0.0f || fMaxY < 0.0f || fMinX >= fRes || fMinY >= fRes)
        continue;

      auto ToTile = [&](float fTexel) -> plUInt32
      { return static_cast<plUInt32>(plMath::Clamp(plMath::Floor(fTexel / fTileSize), 0.0f, static_cast<float>(uiTiles - 1))); };

      rect.m_uiX0 = ToTile(fMinX);
      rect.m_uiY0 = ToTile(fMinY);
      rect.m_uiX1 = ToTile(fMaxX);
      rect.m_uiY1 = ToTile(fMaxY);

      const plUInt32 uiCovered = (rect.m_uiX1 - rect.m_uiX0 + 1) * (rect.m_uiY1 - rect.m_uiY0 + 1);
      if (uiTotal + uiCovered > FLUID_MAX_SOURCE_INDICES)
      {
        bAllFit = false;
        continue;
      }

      uiTotal += uiCovered;
      rect.m_bUsed = true;

      for (plUInt32 y = rect.m_uiY0; y <= rect.m_uiY1; ++y)
        for (plUInt32 x = rect.m_uiX0; x <= rect.m_uiX1; ++x)
          ++counts[y * uiTiles + x];
    }

    plUInt32 uiOffset = 0;
    for (plUInt32 i = 0; i < uiTiles * uiTiles; ++i)
    {
      out_ranges[i].Set(uiOffset, 0);
      uiOffset += counts[i];
    }

    out_indices.SetCountUninitialized(uiTotal);

    for (plUInt32 s = 0; s < rects.GetCount(); ++s)
    {
      const TileRect& rect = rects[s];
      if (!rect.m_bUsed)
        continue;

      for (plUInt32 y = rect.m_uiY0; y <= rect.m_uiY1; ++y)
      {
        for (plUInt32 x = rect.m_uiX0; x <= rect.m_uiX1; ++x)
        {
          plVec2U32& range = out_ranges[y * uiTiles + x];
          out_indices[range.x + range.y] = s;
          ++range.y;
        }
      }
    }

    return bAllFit;
  }
} // namespace

struct plFluidGpuManager::Data
{
  struct Volume
  {
    plFluidVolumeInput m_Input;
    plUInt64 m_uiSubmitFrame = 0;

    plGALTextureHandle m_hState[2];
    plGALTextureHandle m_hPressure[2];
    plGALTextureHandle m_hDivergence;
    plGALTextureHandle m_hObstacle;
    plGALBufferHandle m_hSources;
    plGALBufferHandle m_hSourceTiles;   ///< per tile (first index, count) into m_hSourceIndices
    plGALBufferHandle m_hSourceIndices; ///< the tiles' source lists, back to back
    plGALBufferHandle m_hColliders;
    plConstantBufferStorageHandle m_hConstants;

    /// Water: the aux pair, RGBA16F (wet map, surface pressure head). Written once per frame by the Water inject pass.
    plGALTextureHandle m_hAux[2];
    plUInt32 m_uiAuxCurrent = 0; ///< which of m_hAux holds the live aux state

    plUInt32 m_uiCurrent = 0;      ///< which of m_hState holds the live state
    plUInt32 m_uiResolution = 0;   ///< resolution the textures were created at
    plUInt8 m_uiMode = 0xFF;       ///< mode the textures were created for
    bool m_bObstacleValid = false; ///< false forces the obstacle pass, which a fresh texture always needs

    /// The bed texture, created with its data from m_pBedSource. A reference is kept so a new bed can never reuse the
    /// address of the one the texture was made from.
    plGALTextureHandle m_hBed;
    plSharedPtr<const plFluidBed> m_pBedSource;

    /// Gas smoke: a VDB volume resource owning one 3D texture, which the extrude pass rewrites every step.
    plVDBVolumeResourceHandle m_hSmokeVolume;
    plGALTextureHandle m_hSmokeTexture; ///< the resource's texture; the resource owns and destroys it
    plUInt32 m_uiSmokeDepth = 0;
    plUInt32 m_uiSmokeRes = 0;

    /// Snow drift activity and Water surface statistics: the state reduced to a 16x16 RGBA32F grid (FluidActivity.plShader)
    /// and read back to the CPU. The helper is a raw pointer because volumes are copied in and out of the id table;
    /// DestroyVolumeResources owns its release.
    plGALTextureHandle m_hActivity;
    plGALReadbackTextureHelper* m_pActivityReadback = nullptr;
    bool m_bActivityPending = false;    ///< a readback was issued and has not been consumed yet
    plUInt64 m_uiActivityIssueFrame = 0; ///< render frame the last readback was issued in
    plUInt32 m_uiActivityFrame = 0;      ///< readbacks consumed into m_Activity
    plVec4 m_Activity[FluidActivityCells] = {};

    plVec2U32 m_vAtlasOffset = plVec2U32::MakeZero();
    plVec4 m_AtlasRect = plVec4::MakeZero();

    float m_fIdleSeconds = 0.0f;
    plFluidVolumeStats m_Stats;
  };

  struct World
  {
    plIdTable<plFluidVolumeId, Volume> m_Volumes;

    plGALTextureHandle m_hAtlas[3]; ///< velocity, density, surface
    plGALTextureHandle m_hTable;
    plConstantBufferStorageHandle m_hConstants;

    plUInt32 m_uiAtlasSize = 0;
    plUInt32 m_uiIdleFrames = 0;

    /// Set when the atlas or table was (re)created this frame. On the threaded render path the fluid command list is
    /// submitted after the view graphs, so views of this frame would read them before anything wrote them. Such a
    /// frame publishes no volumes; the solver still fills them, and the next frame reads valid data.
    bool m_bFreshResources = false;
  };

  plMutex m_Mutex;
  plHashTable<plUInt32, World*> m_Worlds;

  plShaderResourceHandle m_hClear;
  plShaderResourceHandle m_hObstacles;
  plShaderResourceHandle m_hInject;
  plShaderResourceHandle m_hAdvect;
  plShaderResourceHandle m_hForces;
  plShaderResourceHandle m_hDivergence;
  plShaderResourceHandle m_hPressure;
  plShaderResourceHandle m_hProject;
  plShaderResourceHandle m_hWaterInject;
  plShaderResourceHandle m_hWaterVelocity;
  plShaderResourceHandle m_hWaterHeight;
  plShaderResourceHandle m_hSnow;
  plShaderResourceHandle m_hFinalize;
  plShaderResourceHandle m_hSmokeExtrude;
  plShaderResourceHandle m_hActivity;

  /// Activity readback helpers of destroyed volumes whose readback was still in flight. Deleted once their fence is
  /// no longer pending, so a readback texture is never released under a copy the GPU has not finished.
  plDynamicArray<plGALReadbackTextureHelper*> m_RetiredReadbacks;

  /// 1x1 deep-water bed bound for every volume without a baked one. Created on first use, destroyed at shutdown.
  plGALTextureHandle m_hBedPlaceholder;

  /// 1x1 zero aux texture Finalize reads for every mode but Water. Created on first use, destroyed at shutdown.
  plGALTextureHandle m_hAuxPlaceholder;

  plUInt32 m_uiSmokeGeneration = 0; ///< makes every smoke resource id unique, so a recreated one never reuses an id

  World* FindWorld(plUInt32 uiWorldIndex);
  World& GetOrCreateWorld(plUInt32 uiWorldIndex);

  void UpdateWorld(plUInt32 uiWorldIndex, World& world);

  /// \brief Shelf packs the volumes into the smallest square atlas that holds them, and drops the ones past the
  /// size cap from the array. Returns the atlas edge length, 0 when nothing is active.
  plUInt32 PackAtlas(plDynamicArray<Volume*>& ref_activeVolumes);

  void EnsureWorldResources(World& world, plUInt32 uiAtlasSize);
  void EnsureVolumeResources(Volume& volume, plUInt32 uiResolution, plUInt8 uiMode, float fInitialDepth, plGALCommandEncoder* pEncoder, plRenderContext* pContext);
  void DestroyVolumeResources(Volume& volume, bool bDestroyPersistent);

  /// \brief Recreates the volume's bed texture when the submitted bed differs from the one it was made from, and
  /// invalidates the obstacle mask then. Returns the texture to bind: the volume's bed or the placeholder.
  plGALTextureHandle EnsureBedResources(Volume& volume);
  void DestroyBedResources(Volume& volume);

  /// \brief The 1x1 zero aux texture, created on first use.
  plGALTextureHandle GetAuxPlaceholder();
  void EnsureSmokeResources(Volume& volume, plUInt32 uiWorldIndex);
  void DestroySmokeResources(Volume& volume);
  void DestroyActivityResources(Volume& volume);
  void ReleaseRetiredReadbacks(bool bForce);
  void ConsumeActivityReadback(Volume& volume);
  void StepActivity(Volume& volume, bool bStepped, plGALCommandEncoder* pEncoder, plRenderContext* pContext);
  void DestroyWorldResources(World& world);
  void RetireWorldResources(World& world);
  void StepVolume(Volume& volume, World& world, plGALCommandEncoder* pEncoder, plRenderContext* pContext);
  void WriteTable(World& world, plArrayPtr<Volume* const> activeVolumes, plGALCommandEncoder* pEncoder);
};

plFluidGpuManager::Data* plFluidGpuManager::s_pData = nullptr;

//////////////////////////////////////////////////////////////////////////
// lifetime

void plFluidGpuManager::OnStartup()
{
  s_pData = PL_DEFAULT_NEW(Data);

  s_pData->m_hClear = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidClear.plShader");
  s_pData->m_hObstacles = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidObstacles.plShader");
  s_pData->m_hInject = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidInject.plShader");
  s_pData->m_hAdvect = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidAdvect.plShader");
  s_pData->m_hForces = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidForces.plShader");
  s_pData->m_hDivergence = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidDivergence.plShader");
  s_pData->m_hPressure = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidPressure.plShader");
  s_pData->m_hProject = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidProject.plShader");
  s_pData->m_hWaterInject = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidWaterInject.plShader");
  s_pData->m_hWaterVelocity = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidWaterVelocity.plShader");
  s_pData->m_hWaterHeight = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidWaterHeight.plShader");
  s_pData->m_hSnow = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidSnow.plShader");
  s_pData->m_hFinalize = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidFinalize.plShader");
  s_pData->m_hSmokeExtrude = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidSmokeExtrude.plShader");
  s_pData->m_hActivity = plResourceManager::LoadResource<plShaderResource>("Shaders/Fluid/FluidActivity.plShader");

  plRenderWorld::GetRenderEvent().AddEventHandler(OnRenderEvent);

  // Every view binds its world's atlas and volume table here, the same slot the engine binds the wind field in.
  plRenderWorld::GetFrameBindingEvent().AddEventHandler(OnFrameBindingEvent);
}

void plFluidGpuManager::OnShutdown()
{
  if (s_pData == nullptr)
    return;

  plRenderWorld::GetRenderEvent().RemoveEventHandler(OnRenderEvent);
  plRenderWorld::GetFrameBindingEvent().RemoveEventHandler(OnFrameBindingEvent);

  for (auto it = s_pData->m_Worlds.GetIterator(); it.IsValid(); ++it)
  {
    s_pData->DestroyWorldResources(*it.Value());
    PL_DEFAULT_DELETE(it.Value());
  }

  s_pData->ReleaseRetiredReadbacks(true);

  if (!s_pData->m_hBedPlaceholder.IsInvalidated())
  {
    plGALDevice::GetDefaultDevice()->DestroyTexture(s_pData->m_hBedPlaceholder);
    s_pData->m_hBedPlaceholder.Invalidate();
  }

  if (!s_pData->m_hAuxPlaceholder.IsInvalidated())
  {
    plGALDevice::GetDefaultDevice()->DestroyTexture(s_pData->m_hAuxPlaceholder);
    s_pData->m_hAuxPlaceholder.Invalidate();
  }

  PL_DEFAULT_DELETE(s_pData);
}

bool plFluidGpuManager::IsEnabled()
{
  return cvar_FluidEnable;
}

plFluidGpuManager::Data::World* plFluidGpuManager::Data::FindWorld(plUInt32 uiWorldIndex)
{
  World** ppWorld = nullptr;
  if (m_Worlds.TryGetValue(uiWorldIndex, ppWorld))
    return *ppWorld;

  return nullptr;
}

plFluidGpuManager::Data::World& plFluidGpuManager::Data::GetOrCreateWorld(plUInt32 uiWorldIndex)
{
  World** ppWorld = nullptr;
  if (m_Worlds.TryGetValue(uiWorldIndex, ppWorld))
    return **ppWorld;

  World* pWorld = PL_DEFAULT_NEW(World);
  m_Worlds.Insert(uiWorldIndex, pWorld);
  return *pWorld;
}

//////////////////////////////////////////////////////////////////////////
// world side

plFluidVolumeHandle plFluidGpuManager::AllocateVolume(plUInt32 uiWorldIndex)
{
  if (s_pData == nullptr)
    return {};

  PL_LOCK(s_pData->m_Mutex);

  Data::World& world = s_pData->GetOrCreateWorld(uiWorldIndex);
  return plFluidVolumeHandle(world.m_Volumes.Insert(Data::Volume()));
}

void plFluidGpuManager::Free(plUInt32 uiWorldIndex, plFluidVolumeHandle& inout_hVolume)
{
  if (s_pData == nullptr || inout_hVolume.IsInvalidated())
    return;

  PL_LOCK(s_pData->m_Mutex);

  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
  {
    Data::Volume oldVolume;
    if (pWorld->m_Volumes.Remove(inout_hVolume.GetInternalID(), &oldVolume))
    {
      s_pData->DestroyVolumeResources(oldVolume, true);
    }
  }

  inout_hVolume.Invalidate();
}

void plFluidGpuManager::SubmitVolume(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, plFluidVolumeInput&& input)
{
  if (s_pData == nullptr || hVolume.IsInvalidated())
    return;

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  if (pWorld == nullptr)
    return;

  Data::Volume* pVolume = nullptr;
  if (!pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume))
    return;

  pVolume->m_Input = std::move(input);
  pVolume->m_uiSubmitFrame = plRenderWorld::GetFrameCounter();
}

void plFluidGpuManager::RemoveWorld(plUInt32 uiWorldIndex)
{
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);

  // The entry and its constants storage stay alive for the same reason plWindFieldManager keeps its own: render
  // contexts hold the storage at frame scope past this call, so it must stay valid and read as "no volumes" rather
  // than return to the shared pool where another owner would overwrite it. A world reusing this index takes over.
  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
  {
    s_pData->RetireWorldResources(*pWorld);
  }
}

//////////////////////////////////////////////////////////////////////////
// render side bindings

void plFluidGpuManager::BindForWorld(plRenderContext* pRenderContext, plUInt32 uiWorldIndex)
{
  if (s_pData == nullptr || pRenderContext == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);

  if (pWorld == nullptr || pWorld->m_hConstants.IsInvalidated() || pWorld->m_hTable.IsInvalidated())
  {
    pRenderContext->SetFrameConstantBuffer("plFluidConstants", plConstantBufferStorageHandle());
    pRenderContext->SetFrameTexture("FluidVolumeTable", plGALTextureHandle());
    pRenderContext->SetFrameTexture("FluidAtlasVelocity", plGALTextureHandle());
    pRenderContext->SetFrameTexture("FluidAtlasDensity", plGALTextureHandle());
    pRenderContext->SetFrameTexture("FluidAtlasSurface", plGALTextureHandle());
    return;
  }

  pRenderContext->SetFrameConstantBuffer("plFluidConstants", pWorld->m_hConstants);
  pRenderContext->SetFrameTexture("FluidVolumeTable", pWorld->m_hTable);
  pRenderContext->SetFrameTexture("FluidAtlasVelocity", pWorld->m_hAtlas[0]);
  pRenderContext->SetFrameTexture("FluidAtlasDensity", pWorld->m_hAtlas[1]);
  pRenderContext->SetFrameTexture("FluidAtlasSurface", pWorld->m_hAtlas[2]);
}

void plFluidGpuManager::OnFrameBindingEvent(const plRenderWorldFrameBindingEvent& e)
{
  BindForWorld(e.m_pRenderContext, e.m_uiWorldIndex);
}

plGALTextureHandle plFluidGpuManager::GetAtlasTexture(plUInt32 uiWorldIndex, plUInt32 uiChannel)
{
  if (s_pData == nullptr || uiChannel > 2)
    return {};

  PL_LOCK(s_pData->m_Mutex);

  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
    return pWorld->m_hAtlas[uiChannel];

  return {};
}

plVec4 plFluidGpuManager::GetAtlasRect(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume)
{
  if (s_pData == nullptr || hVolume.IsInvalidated())
    return plVec4::MakeZero();

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Volume* pVolume = nullptr;
  if (pWorld != nullptr && pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume))
    return pVolume->m_AtlasRect;

  return plVec4::MakeZero();
}

plFluidVolumeStats plFluidGpuManager::GetVolumeStats(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume)
{
  if (s_pData == nullptr || hVolume.IsInvalidated())
    return {};

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Volume* pVolume = nullptr;
  if (pWorld != nullptr && pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume))
    return pVolume->m_Stats;

  return {};
}

plVDBVolumeResourceHandle plFluidGpuManager::GetSmokeVolume(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume)
{
  if (s_pData == nullptr || hVolume.IsInvalidated())
    return {};

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Volume* pVolume = nullptr;
  if (cvar_FluidSmokeVolumes && pWorld != nullptr && pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume) && pVolume->m_uiMode == plFluidMode::Gas &&
      !pVolume->m_hSmokeTexture.IsInvalidated())
    return pVolume->m_hSmokeVolume;

  return {};
}

bool plFluidGpuManager::GetSnowActivity(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, plArrayPtr<float> out_activity, plUInt32& out_uiFrame)
{
  if (s_pData == nullptr || hVolume.IsInvalidated() || out_activity.GetCount() < FluidActivityCells)
    return false;

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Volume* pVolume = nullptr;
  if (pWorld == nullptr || !pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume))
    return false;

  // A frozen or sleeping volume keeps the last grid it read back, which no longer describes anything moving.
  if (pVolume->m_uiMode != plFluidMode::Snow || !pVolume->m_Stats.m_bActive || pVolume->m_Stats.m_bAsleep || pVolume->m_uiActivityFrame == 0)
    return false;

  for (plUInt32 i = 0; i < FluidActivityCells; ++i)
    out_activity[i] = pVolume->m_Activity[i].x;

  out_uiFrame = pVolume->m_uiActivityFrame;
  return true;
}

bool plFluidGpuManager::GetWaterStats(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, plFluidWaterStats& out_stats)
{
  if (s_pData == nullptr || hVolume.IsInvalidated())
    return false;

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Volume* pVolume = nullptr;
  if (pWorld == nullptr || !pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume))
    return false;

  if (pVolume->m_uiMode != plFluidMode::Water || pVolume->m_uiActivityFrame == 0)
    return false;

  out_stats = plFluidWaterStats();
  out_stats.m_fLowestSurface = plMath::MaxValue<float>();
  out_stats.m_fHighestSurface = -plMath::MaxValue<float>();

  for (const plVec4& cell : pVolume->m_Activity)
  {
    out_stats.m_fLowestSurface = plMath::Min(out_stats.m_fLowestSurface, cell.x);
    out_stats.m_fHighestSurface = plMath::Max(out_stats.m_fHighestSurface, cell.y);
    out_stats.m_fFastestFlow = plMath::Max(out_stats.m_fFastestFlow, cell.z);
    out_stats.m_fInvalidTexels += cell.w;
  }

  // The cells in the middle of the grid, which is where a vortex sits.
  const plUInt32 uiMid = SnowActivityGridSize / 2;
  out_stats.m_fCentreSurface = plMath::Min(plMath::Min(pVolume->m_Activity[(uiMid - 1) * SnowActivityGridSize + uiMid - 1].x, pVolume->m_Activity[(uiMid - 1) * SnowActivityGridSize + uiMid].x),
    plMath::Min(pVolume->m_Activity[uiMid * SnowActivityGridSize + uiMid - 1].x, pVolume->m_Activity[uiMid * SnowActivityGridSize + uiMid].x));
  out_stats.m_uiReadback = pVolume->m_uiActivityFrame;
  return true;
}

bool plFluidGpuManager::GetWaterActivityAround(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, const plVec2& vUv, const plVec2& vRadiusUv, plVec4& out_cell, plUInt32& out_uiFrame)
{
  if (s_pData == nullptr || hVolume.IsInvalidated())
    return false;

  PL_LOCK(s_pData->m_Mutex);

  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Volume* pVolume = nullptr;
  if (pWorld == nullptr || !pWorld->m_Volumes.TryGetValue(hVolume.GetInternalID(), pVolume))
    return false;

  if (pVolume->m_uiMode != plFluidMode::Water || !pVolume->m_Stats.m_bActive || pVolume->m_Stats.m_bAsleep || pVolume->m_uiActivityFrame == 0)
    return false;

  // Cells are stored row by row, u along the volume's local x.
  auto CellIndex = [](float fUv) { return static_cast<plInt32>(plMath::Clamp(fUv, 0.0f, 0.9999f) * SnowActivityGridSize); };
  const plInt32 iMinX = CellIndex(vUv.x - vRadiusUv.x);
  const plInt32 iMaxX = CellIndex(vUv.x + vRadiusUv.x);
  const plInt32 iMinY = CellIndex(vUv.y - vRadiusUv.y);
  const plInt32 iMaxY = CellIndex(vUv.y + vRadiusUv.y);

  out_cell.Set(plMath::MaxValue<float>(), -plMath::MaxValue<float>(), 0.0f, 0.0f);
  for (plInt32 y = iMinY; y <= iMaxY; ++y)
  {
    for (plInt32 x = iMinX; x <= iMaxX; ++x)
    {
      const plVec4& cell = pVolume->m_Activity[y * SnowActivityGridSize + x];
      out_cell.x = plMath::Min(out_cell.x, cell.x);
      out_cell.y = plMath::Max(out_cell.y, cell.y);
      out_cell.z = plMath::Max(out_cell.z, cell.z);
      out_cell.w += cell.w;
    }
  }

  out_uiFrame = pVolume->m_uiActivityFrame;
  return true;
}

//////////////////////////////////////////////////////////////////////////
// resources

void plFluidGpuManager::Data::EnsureWorldResources(World& world, plUInt32 uiAtlasSize)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  if (world.m_hConstants.IsInvalidated())
  {
    world.m_hConstants = plRenderContext::CreateConstantBufferStorage<plFluidConstants>();
    plFluidConstants* pConstants = plRenderContext::GetConstantBufferData<plFluidConstants>(world.m_hConstants);
    plMemoryUtils::ZeroFill(pConstants, 1);
  }

  if (world.m_hTable.IsInvalidated())
  {
    plGALTextureCreationDescription desc;
    desc.m_uiWidth = FLUID_TABLE_ROWS;
    desc.m_uiHeight = FLUID_MAX_VOLUMES;
    desc.m_Type = plGALTextureType::Texture2D;
    desc.m_Format = plGALResourceFormat::RGBAFloat;
    desc.m_bAllowShaderResourceView = true;
    desc.m_ResourceAccess.m_bImmutable = false;

    world.m_hTable = pDevice->CreateTexture(desc);
    world.m_bFreshResources = true;
  }

  if (world.m_uiAtlasSize == uiAtlasSize && !world.m_hAtlas[0].IsInvalidated())
    return;

  for (plUInt32 i = 0; i < 3; ++i)
  {
    if (!world.m_hAtlas[i].IsInvalidated())
    {
      pDevice->DestroyTexture(world.m_hAtlas[i]);
      world.m_hAtlas[i].Invalidate();
    }
  }

  if (uiAtlasSize == 0)
  {
    world.m_uiAtlasSize = 0;
    return;
  }

  const plGALResourceFormat::Enum formats[3] = {plGALResourceFormat::RGHalf, plGALResourceFormat::RGBAHalf, plGALResourceFormat::RGBAHalf};

  for (plUInt32 i = 0; i < 3; ++i)
  {
    plGALTextureCreationDescription desc;
    desc.m_uiWidth = uiAtlasSize;
    desc.m_uiHeight = uiAtlasSize;
    desc.m_Type = plGALTextureType::Texture2D;
    desc.m_Format = formats[i];
    desc.m_bAllowShaderResourceView = true;
    desc.m_bAllowUAV = true;
    desc.m_ResourceAccess.m_bImmutable = false;

    world.m_hAtlas[i] = pDevice->CreateTexture(desc);
  }

  world.m_uiAtlasSize = uiAtlasSize;
  world.m_bFreshResources = true;
}

void plFluidGpuManager::Data::EnsureVolumeResources(Volume& volume, plUInt32 uiResolution, plUInt8 uiMode, float fInitialDepth, plGALCommandEncoder* pEncoder, plRenderContext* pContext)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  const bool bNeedsPressure = uiMode == plFluidMode::Gas;
  const bool bWater = uiMode == plFluidMode::Water;

  if (volume.m_hConstants.IsInvalidated())
  {
    volume.m_hConstants = plRenderContext::CreateConstantBufferStorage<plFluidSolverConstants>();
  }

  if (volume.m_hSources.IsInvalidated())
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plFluidSourceGpu);
    desc.m_uiTotalSize = sizeof(plFluidSourceGpu) * FLUID_MAX_SOURCES;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;

    volume.m_hSources = pDevice->CreateBuffer(desc);
  }

  if (volume.m_hSourceTiles.IsInvalidated())
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plVec2U32);
    desc.m_uiTotalSize = sizeof(plVec2U32) * FLUID_SOURCE_TILES * FLUID_SOURCE_TILES;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;

    volume.m_hSourceTiles = pDevice->CreateBuffer(desc);
  }

  if (volume.m_hSourceIndices.IsInvalidated())
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plUInt32);
    desc.m_uiTotalSize = sizeof(plUInt32) * FLUID_MAX_SOURCE_INDICES;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;

    volume.m_hSourceIndices = pDevice->CreateBuffer(desc);
  }

  if (volume.m_hColliders.IsInvalidated())
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = sizeof(plFluidColliderGpu);
    desc.m_uiTotalSize = sizeof(plFluidColliderGpu) * FLUID_MAX_COLLIDERS;
    desc.m_BufferFlags = plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource;
    desc.m_ResourceAccess.m_bImmutable = false;

    volume.m_hColliders = pDevice->CreateBuffer(desc);
  }

  if (volume.m_uiResolution == uiResolution && volume.m_uiMode == uiMode && !volume.m_hState[0].IsInvalidated())
    return;

  // Resolution or mode changed: the old grid cannot be reinterpreted, so the volume restarts from an empty state.
  DestroyVolumeResources(volume, false);

  plGALTextureCreationDescription stateDesc;
  stateDesc.m_uiWidth = uiResolution;
  stateDesc.m_uiHeight = uiResolution;
  stateDesc.m_Type = plGALTextureType::Texture2D;
  // Water's surface changes by fractions of a millimetre per sub-step, which half floats would round away.
  stateDesc.m_Format = bWater ? plGALResourceFormat::RGBAFloat : plGALResourceFormat::RGBAHalf;
  stateDesc.m_bAllowShaderResourceView = true;
  stateDesc.m_bAllowUAV = true;
  stateDesc.m_ResourceAccess.m_bImmutable = false;

  volume.m_hState[0] = pDevice->CreateTexture(stateDesc);
  volume.m_hState[1] = pDevice->CreateTexture(stateDesc);

  plGALTextureCreationDescription scalarDesc = stateDesc;
  scalarDesc.m_Format = plGALResourceFormat::RFloat;

  volume.m_hObstacle = pDevice->CreateTexture(scalarDesc);

  if (bNeedsPressure)
  {
    volume.m_hPressure[0] = pDevice->CreateTexture(scalarDesc);
    volume.m_hPressure[1] = pDevice->CreateTexture(scalarDesc);
    volume.m_hDivergence = pDevice->CreateTexture(scalarDesc);
  }

  if (bWater)
  {
    plGALTextureCreationDescription auxDesc = stateDesc;
    auxDesc.m_Format = plGALResourceFormat::RGBAHalf;

    volume.m_hAux[0] = pDevice->CreateTexture(auxDesc);
    volume.m_hAux[1] = pDevice->CreateTexture(auxDesc);
    volume.m_uiAuxCurrent = 0;
  }

  volume.m_uiResolution = uiResolution;
  volume.m_uiMode = uiMode;
  volume.m_uiCurrent = 0;
  volume.m_bObstacleValid = false;

  // A fresh texture holds undefined data. The state pair and Water's aux pair are the only ones read before they are
  // fully written, so they are the only ones that have to be filled; obstacle, divergence and pressure are rewritten
  // every frame. Only Snow starts with anything in it: its depth channel, so a volume has snow to trample from the
  // first frame. Water starts level with the plane, which leaves ground above the plane dry.
  {
    plFluidSolverConstants* pInit = plRenderContext::GetConstantBufferData<plFluidSolverConstants>(volume.m_hConstants);
    plMemoryUtils::ZeroFill(pInit, 1);
    pInit->SolverRes.Set(uiResolution, uiResolution);
    pInit->SolverInvRes.Set(1.0f / uiResolution, 1.0f / uiResolution);
    pInit->SolverInitialState.SetZero();

    if (uiMode == plFluidMode::Snow)
      pInit->SolverInitialState.x = plMath::Max(fInitialDepth, 0.0f);
  }

  const plUInt32 uiGroups = ThreadGroups(uiResolution);

  // Water's initial state is all zeroes, so its aux pair takes the same clear as the state.
  plHybridArray<plGALTextureHandle, 4> clearTargets;
  clearTargets.PushBack(volume.m_hState[0]);
  clearTargets.PushBack(volume.m_hState[1]);
  if (bWater)
  {
    clearTargets.PushBack(volume.m_hAux[0]);
    clearTargets.PushBack(volume.m_hAux[1]);
  }

  for (const plGALTextureHandle& hTarget : clearTargets)
  {
    pEncoder->TextureBarrier(hTarget, plGALResourceState::ShaderRead, plGALResourceState::UnorderedAccessWrite);

    pContext->BeginCompute("Fluid Clear");
    {
      plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
      bindGroup.BindTexture("FluidClearTarget", hTarget);
      pContext->BindShader(m_hClear);
      pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
    }
    pContext->EndCompute();

    pEncoder->TextureBarrier(hTarget, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ShaderRead);
  }
}

void plFluidGpuManager::Data::DestroyVolumeResources(Volume& volume, bool bDestroyPersistent)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  plGALTextureHandle* textures[8] = {&volume.m_hState[0], &volume.m_hState[1], &volume.m_hPressure[0], &volume.m_hPressure[1], &volume.m_hDivergence, &volume.m_hObstacle,
    &volume.m_hAux[0], &volume.m_hAux[1]};

  for (plGALTextureHandle* pHandle : textures)
  {
    if (!pHandle->IsInvalidated())
    {
      pDevice->DestroyTexture(*pHandle);
      pHandle->Invalidate();
    }
  }

  volume.m_uiResolution = 0;
  volume.m_uiMode = 0xFF;
  volume.m_uiCurrent = 0;
  volume.m_uiAuxCurrent = 0;
  volume.m_bObstacleValid = false;

  DestroySmokeResources(volume);
  DestroyActivityResources(volume);

  if (!bDestroyPersistent)
    return;

  // The bed does not depend on the grid resolution or mode, so it survives their changes.
  DestroyBedResources(volume);

  plGALBufferHandle* buffers[3] = {&volume.m_hSources, &volume.m_hSourceTiles, &volume.m_hSourceIndices};
  for (plGALBufferHandle* pHandle : buffers)
  {
    if (!pHandle->IsInvalidated())
    {
      pDevice->DestroyBuffer(*pHandle);
      pHandle->Invalidate();
    }
  }

  if (!volume.m_hColliders.IsInvalidated())
  {
    pDevice->DestroyBuffer(volume.m_hColliders);
    volume.m_hColliders.Invalidate();
  }

  if (!volume.m_hConstants.IsInvalidated())
  {
    plRenderContext::DeleteConstantBufferStorage(volume.m_hConstants);
    volume.m_hConstants.Invalidate();
  }
}

plGALTextureHandle plFluidGpuManager::Data::EnsureBedResources(Volume& volume)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  if (m_hBedPlaceholder.IsInvalidated())
  {
    // Deep water everywhere, the same value the bake stores where a ray finds nothing.
    const float fDeepWater = -100.0f;

    plGALTextureCreationDescription desc;
    desc.m_uiWidth = 1;
    desc.m_uiHeight = 1;
    desc.m_Type = plGALTextureType::Texture2D;
    desc.m_Format = plGALResourceFormat::RFloat;
    desc.m_bAllowShaderResourceView = true;

    plGALSystemMemoryDescription initData;
    initData.m_pData = plMakeByteBlobPtr(reinterpret_cast<const plUInt8*>(&fDeepWater), sizeof(float));
    initData.m_uiRowPitch = sizeof(float);
    initData.m_uiSlicePitch = sizeof(float);

    m_hBedPlaceholder = pDevice->CreateTexture(desc, plMakeArrayPtr(&initData, 1));
  }

  if (volume.m_pBedSource != volume.m_Input.m_pBed)
  {
    DestroyBedResources(volume);
    volume.m_pBedSource = volume.m_Input.m_pBed;

    // The obstacle pass reads the bed to mark land as solid.
    volume.m_bObstacleValid = false;

    const plFluidBed* pBed = volume.m_pBedSource.Borrow();
    if (pBed != nullptr && pBed->m_uiResolution > 0 && pBed->m_Heights.GetCount() == pBed->m_uiResolution * pBed->m_uiResolution)
    {
      plGALTextureCreationDescription desc;
      desc.m_uiWidth = pBed->m_uiResolution;
      desc.m_uiHeight = pBed->m_uiResolution;
      desc.m_Type = plGALTextureType::Texture2D;
      desc.m_Format = plGALResourceFormat::RFloat;
      desc.m_bAllowShaderResourceView = true;

      plGALSystemMemoryDescription initData;
      initData.m_pData = plMakeByteBlobPtr(reinterpret_cast<const plUInt8*>(pBed->m_Heights.GetData()), pBed->m_Heights.GetCount() * sizeof(float));
      initData.m_uiRowPitch = pBed->m_uiResolution * sizeof(float);
      initData.m_uiSlicePitch = initData.m_uiRowPitch * pBed->m_uiResolution;

      volume.m_hBed = pDevice->CreateTexture(desc, plMakeArrayPtr(&initData, 1));
    }
  }

  return volume.m_hBed.IsInvalidated() ? m_hBedPlaceholder : volume.m_hBed;
}

void plFluidGpuManager::Data::DestroyBedResources(Volume& volume)
{
  if (!volume.m_hBed.IsInvalidated())
  {
    plGALDevice::GetDefaultDevice()->DestroyTexture(volume.m_hBed);
    volume.m_hBed.Invalidate();
  }

  volume.m_pBedSource.Clear();
}

plGALTextureHandle plFluidGpuManager::Data::GetAuxPlaceholder()
{
  if (m_hAuxPlaceholder.IsInvalidated())
  {
    // RGBA16F zero is four zero half floats.
    const plUInt16 zero[4] = {0, 0, 0, 0};

    plGALTextureCreationDescription desc;
    desc.m_uiWidth = 1;
    desc.m_uiHeight = 1;
    desc.m_Type = plGALTextureType::Texture2D;
    desc.m_Format = plGALResourceFormat::RGBAHalf;
    desc.m_bAllowShaderResourceView = true;

    plGALSystemMemoryDescription initData;
    initData.m_pData = plMakeByteBlobPtr(reinterpret_cast<const plUInt8*>(zero), sizeof(zero));
    initData.m_uiRowPitch = sizeof(zero);
    initData.m_uiSlicePitch = sizeof(zero);

    m_hAuxPlaceholder = plGALDevice::GetDefaultDevice()->CreateTexture(desc, plMakeArrayPtr(&initData, 1));
  }

  return m_hAuxPlaceholder;
}

void plFluidGpuManager::Data::EnsureSmokeResources(Volume& volume, plUInt32 uiWorldIndex)
{
  const plFluidVolumeInput& input = volume.m_Input;

  if (volume.m_uiMode != plFluidMode::Gas || !input.m_bSmokeVolume || !cvar_FluidSmokeVolumes)
  {
    DestroySmokeResources(volume);
    return;
  }

  const plUInt32 uiSmokeRes = plMath::Min(volume.m_uiResolution / 2, FluidMaxSmokeResolution);
  const plUInt32 uiSmokeDepth = plMath::Clamp<plUInt32>(input.m_uiSmokeDepth, 4, 64);

  // Matching size means the resource was already made for this volume, including when its creation failed: the
  // failure is logged once and the volume stays without smoke rather than retrying every frame.
  if (volume.m_uiSmokeRes == uiSmokeRes && volume.m_uiSmokeDepth == uiSmokeDepth)
    return;

  DestroySmokeResources(volume);

  if (uiSmokeRes == 0)
    return;

  const float fExtentX = plMath::Max(input.m_vExtents.x, 0.01f);
  const float fExtentY = plMath::Max(input.m_vExtents.y, 0.01f);
  const float fHalfThickness = plMath::Max(input.m_fHalfThickness, 0.01f);

  plVDBVolumeResourceDescriptor desc;
  desc.m_Volume.m_LocalBounds = plBoundingBox::MakeFromMinMax(plVec3(-0.5f), plVec3(0.5f));
  desc.m_Volume.m_vVoxelSize.Set(fExtentX / uiSmokeRes, fExtentY / uiSmokeRes, 2.0f * fHalfThickness / uiSmokeDepth);
  desc.m_Volume.m_Channels = plVDBVolumeChannel::Density | plVDBVolumeChannel::Temperature;
  desc.m_Volume.m_fMinDensity = 0.0f;
  desc.m_Volume.m_fMaxDensity = 4.0f;
  desc.m_Volume.m_vResolution.Set(uiSmokeRes, uiSmokeRes, uiSmokeDepth);
  desc.m_Volume.m_uiFrameCount = 1;

  desc.m_DescGAL.m_Type = plGALTextureType::Texture3D;
  desc.m_DescGAL.m_Format = plGALResourceFormat::RGBAHalf;
  desc.m_DescGAL.m_uiWidth = uiSmokeRes;
  desc.m_DescGAL.m_uiHeight = uiSmokeRes;
  desc.m_DescGAL.m_uiDepth = uiSmokeDepth;
  desc.m_DescGAL.m_uiMipLevelCount = 1;
  desc.m_DescGAL.m_uiArraySize = 1;
  desc.m_DescGAL.m_bAllowShaderResourceView = true;
  desc.m_DescGAL.m_bAllowUAV = true;
  desc.m_DescGAL.m_ResourceAccess.m_bImmutable = false;

  desc.m_SamplerDesc.m_MinFilter = plGALTextureFilterMode::Linear;
  desc.m_SamplerDesc.m_MagFilter = plGALTextureFilterMode::Linear;
  desc.m_SamplerDesc.m_MipFilter = plGALTextureFilterMode::Linear;
  desc.m_SamplerDesc.m_AddressU = plImageAddressMode::Clamp;
  desc.m_SamplerDesc.m_AddressV = plImageAddressMode::Clamp;
  desc.m_SamplerDesc.m_AddressW = plImageAddressMode::Clamp;

  // The resource only creates a texture per frame it is given content for. Zeroes, so the first frames read as empty
  // until the extrude pass has written them. RGBA16F is 8 bytes per voxel.
  const plUInt32 uiRowPitch = uiSmokeRes * 8;
  const plUInt32 uiSlicePitch = uiRowPitch * uiSmokeRes;

  plDynamicArray<plUInt8> zeroes;
  zeroes.SetCount(uiSlicePitch * uiSmokeDepth, 0);

  plGALSystemMemoryDescription initData;
  initData.m_pData = plMakeByteBlobPtr(zeroes.GetData(), zeroes.GetCount());
  initData.m_uiRowPitch = uiRowPitch;
  initData.m_uiSlicePitch = uiSlicePitch;

  desc.m_FrameContent.PushBack(plMakeArrayPtr(&initData, 1));

  plStringBuilder sResourceId;
  sResourceId.SetFormat("FluidSmoke-W{}-{}", uiWorldIndex, ++m_uiSmokeGeneration);

  volume.m_hSmokeVolume = plResourceManager::CreateResource<plVDBVolumeResource>(sResourceId, std::move(desc), "Fluid Smoke Volume");

  {
    plResourceLock<plVDBVolumeResource> pSmoke(volume.m_hSmokeVolume, plResourceAcquireMode::PointerOnly);
    volume.m_hSmokeTexture = pSmoke->GetGALTexture(0);
  }

  if (volume.m_hSmokeTexture.IsInvalidated())
  {
    plLog::Error("Fluid: the VDB volume resource '{}' created no texture for a {}x{}x{} RGBA16F UAV volume; smoke is off for this volume", sResourceId, uiSmokeRes, uiSmokeRes, uiSmokeDepth);
  }

  volume.m_uiSmokeRes = uiSmokeRes;
  volume.m_uiSmokeDepth = uiSmokeDepth;
}

void plFluidGpuManager::Data::DestroySmokeResources(Volume& volume)
{
  // The texture belongs to the resource: dropping the last handle lets the resource manager's unused sweep unload it,
  // and its UnloadData hands the texture to plGALDevice::DestroyTexture, which only frees it at the end of the frame.
  volume.m_hSmokeTexture.Invalidate();
  volume.m_hSmokeVolume.Invalidate();
  volume.m_uiSmokeRes = 0;
  volume.m_uiSmokeDepth = 0;
}

void plFluidGpuManager::Data::DestroyActivityResources(Volume& volume)
{
  // The activity texture goes through the device's dead-object list like every other solver texture, so a copy
  // recorded from it earlier completes first.
  if (!volume.m_hActivity.IsInvalidated())
  {
    plGALDevice::GetDefaultDevice()->DestroyTexture(volume.m_hActivity);
    volume.m_hActivity.Invalidate();
  }

  // This can run on the update thread (Free), so the readback helper is neither queried nor released here. It waits
  // in m_RetiredReadbacks, and the render side deletes it once its fence is no longer pending.
  if (volume.m_pActivityReadback != nullptr)
  {
    m_RetiredReadbacks.PushBack(volume.m_pActivityReadback);
    volume.m_pActivityReadback = nullptr;
  }

  volume.m_bActivityPending = false;
  volume.m_uiActivityIssueFrame = 0;
  volume.m_uiActivityFrame = 0;
  plMemoryUtils::ZeroFill(volume.m_Activity, FluidActivityCells);
}

void plFluidGpuManager::Data::ReleaseRetiredReadbacks(bool bForce)
{
  for (plUInt32 i = m_RetiredReadbacks.GetCount(); i > 0; --i)
  {
    plGALReadbackTextureHelper* pHelper = m_RetiredReadbacks[i - 1];
    if (bForce || pHelper->GetReadbackResult(plTime::MakeZero()) != plGALAsyncResult::Pending)
    {
      PL_DEFAULT_DELETE(pHelper);
      m_RetiredReadbacks.RemoveAtAndSwap(i - 1);
    }
  }
}

void plFluidGpuManager::Data::ConsumeActivityReadback(Volume& volume)
{
  if (!volume.m_bActivityPending || volume.m_pActivityReadback == nullptr)
    return;

  const plEnum<plGALAsyncResult> result = volume.m_pActivityReadback->GetReadbackResult(plTime::MakeZero());
  if (result == plGALAsyncResult::Pending)
    return;

  // Ready but unmappable, or expired: drop it. The next readback is issued as if this one had arrived.
  volume.m_bActivityPending = false;

  if (result != plGALAsyncResult::Ready)
    return;

  const plGALTextureSubresource subResource;
  plDynamicArray<plGALSystemMemoryDescription> memory;
  plReadbackTextureLock lock = volume.m_pActivityReadback->LockTexture(plMakeArrayPtr(&subResource, 1), memory);
  if (!lock || memory.IsEmpty())
    return;

  const plGALSystemMemoryDescription& mem = memory[0];
  const plUInt32 uiRowBytes = SnowActivityGridSize * sizeof(plVec4);
  if (mem.m_uiRowPitch < uiRowBytes || mem.m_pData.GetCount() < mem.m_uiRowPitch * (SnowActivityGridSize - 1) + uiRowBytes)
    return;

  for (plUInt32 y = 0; y < SnowActivityGridSize; ++y)
  {
    plMemoryUtils::RawByteCopy(volume.m_Activity + y * SnowActivityGridSize, mem.m_pData.GetPtr() + y * mem.m_uiRowPitch, uiRowBytes);
  }

  ++volume.m_uiActivityFrame;
}

void plFluidGpuManager::Data::StepActivity(Volume& volume, bool bStepped, plGALCommandEncoder* pEncoder, plRenderContext* pContext)
{
  if (volume.m_uiMode != plFluidMode::Snow && volume.m_uiMode != plFluidMode::Water)
    return;

  // A Snow volume that did not step this frame moved no snow; its drift channel still holds the last step's value.
  // A Water volume that did not step keeps the statistics of the surface it froze with.
  if (!bStepped)
  {
    if (volume.m_uiMode == plFluidMode::Snow && !volume.m_bActivityPending && volume.m_uiActivityFrame > 0)
    {
      plMemoryUtils::ZeroFill(volume.m_Activity, FluidActivityCells);
      ++volume.m_uiActivityFrame;
    }
    return;
  }

  // One readback in flight per volume, and at most one every few frames.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (volume.m_bActivityPending || (volume.m_uiActivityIssueFrame != 0 && uiFrame - volume.m_uiActivityIssueFrame < FluidActivityInterval))
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  if (volume.m_hActivity.IsInvalidated())
  {
    plGALTextureCreationDescription desc;
    desc.m_uiWidth = SnowActivityGridSize;
    desc.m_uiHeight = SnowActivityGridSize;
    desc.m_Type = plGALTextureType::Texture2D;
    desc.m_Format = plGALResourceFormat::RGBAFloat;
    desc.m_bAllowShaderResourceView = true;
    desc.m_bAllowUAV = true;
    desc.m_ResourceAccess.m_bImmutable = false;

    volume.m_hActivity = pDevice->CreateTexture(desc);
    if (volume.m_hActivity.IsInvalidated())
      return;
  }

  if (volume.m_pActivityReadback == nullptr)
    volume.m_pActivityReadback = PL_DEFAULT_NEW(plGALReadbackTextureHelper);

  pEncoder->TextureBarrier(volume.m_hActivity, plGALResourceState::ShaderRead, plGALResourceState::UnorderedAccessWrite);
  pContext->BeginCompute("Fluid Activity");
  {
    plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
    bindGroup.BindTexture("FluidStateInput", volume.m_hState[volume.m_uiCurrent]);
    bindGroup.BindTexture("FluidActivityOutput", volume.m_hActivity);
    pContext->BindShader(m_hActivity);
    pContext->Dispatch(1, 1, 1).IgnoreResult();
  }
  pContext->EndCompute();
  pEncoder->TextureBarrier(volume.m_hActivity, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ShaderRead);

  if (volume.m_pActivityReadback->ReadbackTexture(*pEncoder, volume.m_hActivity) != 0)
  {
    volume.m_bActivityPending = true;
    volume.m_uiActivityIssueFrame = uiFrame;
  }
}

void plFluidGpuManager::Data::DestroyWorldResources(World& world)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (auto it = world.m_Volumes.GetIterator(); it.IsValid(); ++it)
  {
    DestroyVolumeResources(it.Value(), true);
  }
  world.m_Volumes.Clear();

  for (plUInt32 i = 0; i < 3; ++i)
  {
    if (!world.m_hAtlas[i].IsInvalidated())
    {
      pDevice->DestroyTexture(world.m_hAtlas[i]);
      world.m_hAtlas[i].Invalidate();
    }
  }

  if (!world.m_hTable.IsInvalidated())
  {
    pDevice->DestroyTexture(world.m_hTable);
    world.m_hTable.Invalidate();
  }

  if (!world.m_hConstants.IsInvalidated())
  {
    plRenderContext::DeleteConstantBufferStorage(world.m_hConstants);
    world.m_hConstants.Invalidate();
  }

  world.m_uiAtlasSize = 0;
}

void plFluidGpuManager::Data::RetireWorldResources(World& world)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (auto it = world.m_Volumes.GetIterator(); it.IsValid(); ++it)
  {
    DestroyVolumeResources(it.Value(), true);
  }
  world.m_Volumes.Clear();

  for (plUInt32 i = 0; i < 3; ++i)
  {
    if (!world.m_hAtlas[i].IsInvalidated())
    {
      pDevice->DestroyTexture(world.m_hAtlas[i]);
      world.m_hAtlas[i].Invalidate();
    }
  }

  if (!world.m_hTable.IsInvalidated())
  {
    pDevice->DestroyTexture(world.m_hTable);
    world.m_hTable.Invalidate();
  }

  // Zeroed constants read as "no volumes" wherever the storage is still bound at frame scope.
  if (!world.m_hConstants.IsInvalidated())
  {
    plFluidConstants* pConstants = plRenderContext::GetConstantBufferData<plFluidConstants>(world.m_hConstants);
    plMemoryUtils::ZeroFill(pConstants, 1);
  }

  world.m_uiAtlasSize = 0;
}

//////////////////////////////////////////////////////////////////////////
// solver

void plFluidGpuManager::Data::StepVolume(Volume& volume, World& world, plGALCommandEncoder* pEncoder, plRenderContext* pContext)
{
  const plFluidVolumeInput& input = volume.m_Input;
  const plFluidSettings& settings = input.m_Settings;

  const plUInt32 uiRes = volume.m_uiResolution;
  const plUInt32 uiGroups = ThreadGroups(uiRes);
  const plUInt8 uiMode = volume.m_uiMode;
  const bool bHasPressure = uiMode == plFluidMode::Gas;
  const bool bWater = uiMode == plFluidMode::Water;

  PL_PROFILE_AND_MARKER(pEncoder, "Fluid Volume");

  // One step per frame, clamped. Semi-Lagrangian advection is unconditionally stable and Water sub-steps within its
  // own stability limit, so a long frame simulates in slow motion rather than blowing up.
  float fStepTime = static_cast<float>(input.m_DeltaTime.GetSeconds());
  fStepTime = plMath::Clamp(fStepTime, 0.0f, plMath::Max(cvar_FluidMaxStepTime.GetValue(), 0.001f));

  // A volume with nothing driving it stops stepping once its sources have been gone for long enough. Snow and Water
  // keep going while it snows or rains, because the precipitation is a source of its own. Water also keeps going long
  // enough for its waves to settle, since a frozen swell would hang in mid air.
  const bool bHasInput = !input.m_Sources.IsEmpty() || (uiMode != plFluidMode::Gas && input.m_fPrecipitation > 0.0f);
  const float fSleepAfter = bWater ? plMath::Max(input.m_fSleepAfter, FluidWaterMinSleepAfter) : plMath::Max(input.m_fSleepAfter, 0.0f);

  volume.m_fIdleSeconds = bHasInput ? 0.0f : volume.m_fIdleSeconds + fStepTime;
  const bool bAsleep = volume.m_fIdleSeconds > fSleepAfter;

  volume.m_Stats.m_bAsleep = bAsleep;
  volume.m_Stats.m_uiVramBytes = uiRes * uiRes * (2 * (bWater ? 16 : 8) + (bHasPressure ? 3 * 4 : 0) + (bWater ? 2 * 8 : 0) + 4) + volume.m_uiSmokeRes * volume.m_uiSmokeRes * volume.m_uiSmokeDepth * 8;

  const bool bRunSolver = fStepTime > 0.0f && !bAsleep;

  // Before the obstacle test below: a new bed invalidates the mask.
  const plGALTextureHandle hBed = EnsureBedResources(volume);
  const bool bHasBed = !volume.m_hBed.IsInvalidated();

  // Geometry of the grid. Texels are square and sized from the X extent, so a strongly non-square volume
  // simulates anisotropically in Y; keep extents close to square.
  plTransform transform = input.m_Transform;
  transform.m_vScale.Set(1.0f);
  const plTransform inverseTransform = transform.GetInverse();

  const float fExtentX = plMath::Max(input.m_vExtents.x, 0.01f);
  const float fExtentY = plMath::Max(input.m_vExtents.y, 0.01f);
  const float fTexelSize = fExtentX / static_cast<float>(uiRes);
  const float fHalfThickness = plMath::Max(input.m_fHalfThickness, 0.01f);

  auto ToTexel = [&](const plVec3& vWorldPos) -> plVec3
  {
    const plVec3 vLocal = inverseTransform.TransformPosition(vWorldPos);
    return plVec3((vLocal.x / fExtentX + 0.5f) * uiRes, (vLocal.y / fExtentY + 0.5f) * uiRes, vLocal.z);
  };

  // ---- constants -------------------------------------------------------
  plFluidSolverConstants* pConstants = plRenderContext::GetConstantBufferData<plFluidSolverConstants>(volume.m_hConstants);
  plMemoryUtils::ZeroFill(pConstants, 1);

  pConstants->SolverRes.Set(uiRes, uiRes);
  pConstants->SolverInvRes.Set(1.0f / uiRes, 1.0f / uiRes);
  pConstants->SolverTexelSize = fTexelSize;
  pConstants->SolverStepTime = fStepTime;
  pConstants->SolverMode = uiMode;
  pConstants->SolverBoundary = static_cast<plUInt32>(settings.m_Boundary.GetValue());
  pConstants->SolverFlags = settings.m_Advection == plFluidAdvection::MacCormack ? FLUID_FLAG_MACCORMACK : 0u;
  pConstants->SolverSeed = static_cast<plUInt32>(plRenderWorld::GetFrameCounter());
  pConstants->SolverAtlasOffset = volume.m_vAtlasOffset;

  // Dissipation and decay are authored per second; the solver applies them once per step.
  pConstants->SolverDensityDissipation = plMath::Pow(plMath::Clamp(settings.m_fDensityDissipation, 0.0f, 1.0f), fStepTime);
  pConstants->SolverVelocityDissipation = plMath::Pow(plMath::Clamp(settings.m_fVelocityDissipation, 0.0f, 1.0f), fStepTime);
  pConstants->SolverBuoyancy = settings.m_fBuoyancy;
  pConstants->SolverAmbientTemperature = settings.m_fAmbientTemperature;
  pConstants->SolverVorticity = settings.m_fVorticity;
  pConstants->SolverViscosity = settings.m_fViscosity;

  // Water sub-steps. The explicit shallow water step is stable while a wave crosses at most half a texel per sub-step
  // (the wave speed is sqrt(g * depth), bounded because deeper ground counts as Depth), and depths stay positive while
  // no face moves water faster than a quarter texel per sub-step. The count is planned from the wave speed and the
  // fastest flow the sources ask for, and capped by Sub Steps. When the cap bites, the frame simulates less time than
  // passed, in slow motion, with the same physics: changing the depth or the speed limit per frame instead makes a
  // whirlpool breathe and a pond ring into noise whenever the frame rate wobbles, as it does in the editor.
  float fOpenDepth = plMath::Clamp(settings.m_fDepth, 0.05f, 100.0f);
  plUInt32 uiSubSteps = 1;
  float fSubStepTime = fStepTime;
  float fMaxSpeed = 0.0f;

  if (bWater && fStepTime > 0.0f)
  {
    constexpr float fGravity = 9.81f;
    constexpr float fCrestMargin = 0.5f; ///< metres of crest above rest the wave speed estimate allows for

    float fPlannedFlow = FluidWaterMinPlannedFlow;
    for (const plFluidWorldSource& src : input.m_Sources)
    {
      fPlannedFlow = plMath::Max(fPlannedFlow, src.m_vVelocity.GetLength(), plMath::Abs(src.m_fSwirl), src.m_fDrain);
    }

    const float fWaveSpeed = plMath::Sqrt(fGravity * (fOpenDepth + fCrestMargin));
    const float fStableStep = plMath::Max(plMath::Min(0.5f * fTexelSize / fWaveSpeed, 0.25f * fTexelSize / (1.25f * fPlannedFlow)), 1e-5f);
    const plUInt32 uiMaxSubSteps = plMath::Clamp<plUInt32>(settings.m_uiSubSteps, 1, 16);

    uiSubSteps = plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Ceil(fStepTime / fStableStep)), 1, uiMaxSubSteps);
    fSubStepTime = plMath::Min(fStepTime / static_cast<float>(uiSubSteps), fStableStep);
    fStepTime = fSubStepTime * static_cast<float>(uiSubSteps);

    // A guard for flows faster than the sources planned for: the stable limit of the sub-step.
    fMaxSpeed = 0.25f * fTexelSize / fSubStepTime;
  }

  pConstants->SolverStepTime = fStepTime;
  pConstants->SolverSubStepTime = fSubStepTime;
  pConstants->SolverSubSteps = uiSubSteps;
  pConstants->SolverOpenDepth = fOpenDepth;
  pConstants->SolverMaxSpeed = fMaxSpeed;
  pConstants->SolverTime = static_cast<float>(plMath::Mod(input.m_WorldTime.GetSeconds(), 1000.0));

  // Authored per 60 Hz step and per second; the Water kernels apply them once per sub-step.
  pConstants->SolverDamping = plMath::Pow(1.0f - plMath::Clamp(settings.m_fDamping, 0.0f, 1.0f), fSubStepTime * 60.0f);
  pConstants->SolverFoamDecay = plMath::Pow(plMath::Clamp(settings.m_fFoamDecay, 0.0f, 1.0f), fSubStepTime);
  pConstants->SolverFriction = plMath::Max(settings.m_fFriction, 0.0f);
  pConstants->SolverDryDepth = plMath::Max(settings.m_fDryDepth, 0.001f);
  pConstants->SolverWetFade = fStepTime / plMath::Max(settings.m_fWetMemory, 0.1f);
  pConstants->SolverFoamDivergence = plMath::Max(settings.m_fFoamDivergence, 0.0f);
  pConstants->SolverFoamShear = plMath::Max(settings.m_fFoamShear, 0.0f);
  pConstants->SolverChop = plMath::Max(settings.m_fChop, 0.0f);
  pConstants->SolverChopScale = plMath::Max(settings.m_fChopScale, 0.1f);
  // The field changes about as fast as a wave of the chop's size passes, so it keeps raising new waves.
  pConstants->SolverChopRate = plMath::Clamp(plMath::Sqrt(9.81f * fOpenDepth) / plMath::Max(settings.m_fChopScale, 0.1f) * 0.35f, 0.2f, 4.0f);
  pConstants->SolverTurbulence = plMath::Clamp(settings.m_fTurbulence, 0.0f, 1.0f);

  pConstants->SolverRainRate = settings.m_fRainDropRate * input.m_fPrecipitation * fStepTime;
  pConstants->SolverRainRadius = settings.m_fRainDropSize / plMath::Max(fTexelSize, 1e-6f);

  // Snow rates are authored per minute.
  pConstants->SolverAccumulation = settings.m_fAccumulationRate * input.m_fPrecipitation * fStepTime / 60.0f;
  pConstants->SolverMaxDepth = settings.m_fMaxDepth;
  pConstants->SolverCompression = plMath::Clamp(settings.m_fCompression, 0.0f, 1.0f);
  pConstants->SolverRidge = plMath::Clamp(settings.m_fRidge, 0.0f, 1.0f);
  pConstants->SolverRefill = settings.m_fRefillRate * fStepTime / 60.0f;
  pConstants->SolverSlump = plMath::Clamp(settings.m_fSlump, 0.0f, 1.0f);
  pConstants->SolverRepose = plMath::Max(settings.m_fRepose, 0.0f);
  // Settle is authored per second; the kernel consumes the share that packs down in this step.
  pConstants->SolverSettle = plMath::Clamp(settings.m_fSettle * fStepTime, 0.0f, 1.0f);

  const plVec3 vWindLocal = inverseTransform.m_qRotation * input.m_vWind;
  pConstants->SolverWind.Set(vWindLocal.x * settings.m_fWindStrength, vWindLocal.y * settings.m_fWindStrength);

  pConstants->SolverSmokeDims.Set(volume.m_uiSmokeRes, volume.m_uiSmokeRes, volume.m_uiSmokeDepth, 0);
  pConstants->SolverSmokeFalloff = plMath::Max(input.m_fSmokeFalloff, 0.0f);

  // The snow disturbance signal halves every FluidDisturbanceHalfLife seconds, so a footstep reads as a short burst.
  pConstants->SolverDisturbanceDecay = plMath::Pow(0.5f, fStepTime / FluidDisturbanceHalfLife);

  pConstants->SolverShoreFoam = plMath::Max(settings.m_fFoamSurf * input.m_fShoreFoam, 0.0f);
  pConstants->SolverShoreDamping = plMath::Max(input.m_fShoreDamping, 0.0f);
  pConstants->SolverGravity = 9.81f;
  pConstants->SolverHasBed = bHasBed ? 1u : 0u;

  // Authored in metres, consumed in texels.
  pConstants->SolverSpongeWidth = plMath::Max(settings.m_fSpongeWidth / plMath::Max(fTexelSize, 1e-6f), 0.0f);

  // ---- source and collider tables --------------------------------------
  plHybridArray<plFluidSourceGpu, 16> sources;
  for (const plFluidWorldSource& src : input.m_Sources)
  {
    if (sources.GetCount() >= FLUID_MAX_SOURCES)
      break;

    const plVec3 vTexel = ToTexel(src.m_vPosition);
    const plVec3 vPrevTexel = ToTexel(src.m_vPrevPosition);
    const plVec3 vVelocityLocal = inverseTransform.m_qRotation * src.m_vVelocity;

    plFluidSourceGpu& gpu = sources.ExpandAndGetRef();
    gpu.Position.Set(vTexel.x, vTexel.y);
    gpu.PrevPosition.Set(vPrevTexel.x, vPrevTexel.y);
    gpu.Velocity.Set(vVelocityLocal.x, vVelocityLocal.y);
    gpu.Radius = plMath::Max(src.m_fRadius / fTexelSize, 0.5f);
    gpu.Density = src.m_fDensity;
    gpu.Temperature = src.m_fTemperature;
    gpu.HeightImpulse = src.m_fHeightImpulse;
    gpu.Press = src.m_fPress;
    gpu.Shape = static_cast<float>(src.m_uiShape);
    gpu.Extents.Set(plMath::Max(src.m_vExtents.x / fTexelSize, 0.5f), plMath::Max(src.m_vExtents.y / fTexelSize, 0.5f));
    gpu.Falloff = plMath::Clamp(src.m_fFalloff, 0.0f, 1.0f);
    gpu.HeightFade = plMath::Clamp(plMath::Abs(vTexel.z) / fHalfThickness, 0.0f, 1.0f);
    gpu.Swirl = src.m_fSwirl;
    gpu.Drain = src.m_fDrain;
    gpu.Axis.Set(0.0f, 0.0f);

    const plVec3 vAxisLocal = inverseTransform.m_qRotation * src.m_vAxis;
    if (src.m_uiShape == plFluidEmitterShape::Box)
    {
      // Unit in-plane direction of the box's local X; zero keeps the box grid aligned.
      plVec2 vDir(vAxisLocal.x, vAxisLocal.y);
      if (vDir.NormalizeIfNotZero(plVec2::MakeZero(), 1e-4f).Succeeded())
        gpu.Axis = vDir;
    }
    else if (src.m_uiShape == plFluidEmitterShape::Capsule)
    {
      // Half the segment in texels, scaled per axis exactly like ToTexel scales positions.
      gpu.Axis.Set(vAxisLocal.x / fExtentX * uiRes, vAxisLocal.y / fExtentY * uiRes);
    }
  }

  plHybridArray<plFluidColliderGpu, 8> colliders;
  for (const plFluidWorldCollider& col : input.m_Colliders)
  {
    if (colliders.GetCount() >= FLUID_MAX_COLLIDERS)
      break;

    const plVec3 vTexel = ToTexel(col.m_vPosition);
    if (plMath::Abs(vTexel.z) > fHalfThickness)
      continue;

    const plQuat relative = inverseTransform.m_qRotation * col.m_qRotation;
    const plVec3 vLocalX = relative * plVec3(1, 0, 0);

    plFluidColliderGpu& gpu = colliders.ExpandAndGetRef();
    gpu.Position.Set(vTexel.x, vTexel.y);
    gpu.Extents.Set(plMath::Max(col.m_vExtents.x / fTexelSize, 0.5f), plMath::Max(col.m_vExtents.y / fTexelSize, 0.5f));
    gpu.Shape = static_cast<float>(col.m_uiShape);
    gpu.Slip = col.m_fSlip;
    gpu.Rotation = plMath::ATan2(vLocalX.y, vLocalX.x).GetRadian();
    gpu.Padding0 = 0.0f;
  }

  pConstants->SolverNumSources = sources.GetCount();
  pConstants->SolverNumColliders = colliders.GetCount();

  if (!sources.IsEmpty())
    pEncoder->UpdateBuffer(volume.m_hSources, 0, sources.GetArrayPtr().ToByteArray(), plGALUpdateMode::AheadOfTime);

  // The tile lists are uploaded every frame, empty ones included, so no kernel reads last frame's lists.
  {
    plVec2U32 tileRanges[FLUID_SOURCE_TILES * FLUID_SOURCE_TILES];
    plDynamicArray<plUInt32> tileIndices;
    if (!BinSources(sources.GetArrayPtr(), uiRes, plMakeArrayPtr(tileRanges), tileIndices))
    {
      plLog::Warning("Fluid: a volume's sources cover more than {} tile entries; the last ones are left out", FLUID_MAX_SOURCE_INDICES);
    }

    pEncoder->UpdateBuffer(volume.m_hSourceTiles, 0, plMakeArrayPtr(tileRanges).ToByteArray(), plGALUpdateMode::AheadOfTime);
    if (!tileIndices.IsEmpty())
      pEncoder->UpdateBuffer(volume.m_hSourceIndices, 0, tileIndices.GetArrayPtr().ToByteArray(), plGALUpdateMode::AheadOfTime);
  }

  if (!colliders.IsEmpty())
    pEncoder->UpdateBuffer(volume.m_hColliders, 0, colliders.GetArrayPtr().ToByteArray(), plGALUpdateMode::AheadOfTime);

  // ---- passes ----------------------------------------------------------
  auto BeginWrite = [&](plGALTextureHandle hTexture)
  { pEncoder->TextureBarrier(hTexture, plGALResourceState::ShaderRead, plGALResourceState::UnorderedAccessWrite); };

  auto EndWrite = [&](plGALTextureHandle hTexture)
  { pEncoder->TextureBarrier(hTexture, plGALResourceState::UnorderedAccessWrite, plGALResourceState::ShaderRead); };

  plUInt32 uiDispatches = 0;

  // Obstacles: colliders plus the land of the bed. A volume whose colliders never move and whose bed did not change
  // keeps the mask it already has.
  if (bRunSolver && (!volume.m_bObstacleValid || input.m_bCollidersDirty))
  {
    BeginWrite(volume.m_hObstacle);
    pContext->BeginCompute("Fluid Obstacles");
    {
      plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
      bindGroup.BindBuffer("FluidColliders", volume.m_hColliders);
      bindGroup.BindTexture("FluidBedInput", hBed);
      bindGroup.BindTexture("FluidObstacleOutput", volume.m_hObstacle);
      pContext->BindShader(m_hObstacles);
      pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
    }
    pContext->EndCompute();
    EndWrite(volume.m_hObstacle);

    volume.m_bObstacleValid = true;
    ++uiDispatches;
  }

  // Every state pass reads the live texture and writes the other one, then swaps.
  auto StatePass = [&](const char* szName, const plShaderResourceHandle& hShader, bool bBindSources, bool bBindBed)
  {
    const plGALTextureHandle hRead = volume.m_hState[volume.m_uiCurrent];
    const plGALTextureHandle hWrite = volume.m_hState[1 - volume.m_uiCurrent];

    BeginWrite(hWrite);
    pContext->BeginCompute(szName);
    {
      plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
      if (bBindSources)
      {
        bindGroup.BindBuffer("FluidSources", volume.m_hSources);
        bindGroup.BindBuffer("FluidSourceTiles", volume.m_hSourceTiles);
        bindGroup.BindBuffer("FluidSourceIndices", volume.m_hSourceIndices);
      }
      if (bBindBed)
        bindGroup.BindTexture("FluidBedInput", hBed);
      bindGroup.BindTexture("FluidStateInput", hRead);
      bindGroup.BindTexture("FluidObstacleInput", volume.m_hObstacle);
      bindGroup.BindTexture("FluidStateOutput", hWrite);
      pContext->BindShader(hShader);
      pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
    }
    pContext->EndCompute();
    EndWrite(hWrite);

    volume.m_uiCurrent = 1 - volume.m_uiCurrent;
    ++uiDispatches;
  };

  if (bRunSolver && bWater)
  {
    // Sources once per frame, into the state and the aux pair.
    {
      const plGALTextureHandle hRead = volume.m_hState[volume.m_uiCurrent];
      const plGALTextureHandle hWrite = volume.m_hState[1 - volume.m_uiCurrent];
      const plGALTextureHandle hAuxRead = volume.m_hAux[volume.m_uiAuxCurrent];
      const plGALTextureHandle hAuxWrite = volume.m_hAux[1 - volume.m_uiAuxCurrent];

      BeginWrite(hWrite);
      BeginWrite(hAuxWrite);
      pContext->BeginCompute("Fluid Water Inject");
      {
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
        bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
        bindGroup.BindBuffer("FluidSources", volume.m_hSources);
        bindGroup.BindBuffer("FluidSourceTiles", volume.m_hSourceTiles);
        bindGroup.BindBuffer("FluidSourceIndices", volume.m_hSourceIndices);
        bindGroup.BindTexture("FluidStateInput", hRead);
        bindGroup.BindTexture("FluidAuxInput", hAuxRead);
        bindGroup.BindTexture("FluidBedInput", hBed);
        bindGroup.BindTexture("FluidObstacleInput", volume.m_hObstacle);
        bindGroup.BindTexture("FluidStateOutput", hWrite);
        bindGroup.BindTexture("FluidAuxOutput", hAuxWrite);
        pContext->BindShader(m_hWaterInject);
        pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
      }
      pContext->EndCompute();
      EndWrite(hWrite);
      EndWrite(hAuxWrite);

      volume.m_uiCurrent = 1 - volume.m_uiCurrent;
      volume.m_uiAuxCurrent = 1 - volume.m_uiAuxCurrent;
      ++uiDispatches;
    }

    // Then the sub-steps, each a momentum pass on the faces and a mass pass on the texels.
    auto WaterPass = [&](const char* szName, const plShaderResourceHandle& hShader)
    {
      const plGALTextureHandle hRead = volume.m_hState[volume.m_uiCurrent];
      const plGALTextureHandle hWrite = volume.m_hState[1 - volume.m_uiCurrent];

      BeginWrite(hWrite);
      pContext->BeginCompute(szName);
      {
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
        bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
        bindGroup.BindTexture("FluidStateInput", hRead);
        bindGroup.BindTexture("FluidAuxInput", volume.m_hAux[volume.m_uiAuxCurrent]);
        bindGroup.BindTexture("FluidBedInput", hBed);
        bindGroup.BindTexture("FluidObstacleInput", volume.m_hObstacle);
        bindGroup.BindTexture("FluidStateOutput", hWrite);
        pContext->BindShader(hShader);
        pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
      }
      pContext->EndCompute();
      EndWrite(hWrite);

      volume.m_uiCurrent = 1 - volume.m_uiCurrent;
      ++uiDispatches;
    };

    for (plUInt32 i = 0; i < uiSubSteps; ++i)
    {
      WaterPass("Fluid Water Velocity", m_hWaterVelocity);
      WaterPass("Fluid Water Height", m_hWaterHeight);
    }
  }
  else if (bRunSolver)
  {
    StatePass("Fluid Inject", m_hInject, true, false);

    if (uiMode == plFluidMode::Gas)
    {
      StatePass("Fluid Advect", m_hAdvect, false, false);
      StatePass("Fluid Forces", m_hForces, false, false);

      // Divergence, then the Jacobi pressure solve, then the projection that removes it. Divergence also reads the
      // sources, for the vortex drains.
      BeginWrite(volume.m_hDivergence);
      BeginWrite(volume.m_hPressure[0]);
      pContext->BeginCompute("Fluid Divergence");
      {
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
        bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
        bindGroup.BindBuffer("FluidSources", volume.m_hSources);
        bindGroup.BindBuffer("FluidSourceTiles", volume.m_hSourceTiles);
        bindGroup.BindBuffer("FluidSourceIndices", volume.m_hSourceIndices);
        bindGroup.BindTexture("FluidStateInput", volume.m_hState[volume.m_uiCurrent]);
        bindGroup.BindTexture("FluidObstacleInput", volume.m_hObstacle);
        bindGroup.BindTexture("FluidDivergenceOutput", volume.m_hDivergence);
        bindGroup.BindTexture("FluidPressureOutput", volume.m_hPressure[0]);
        pContext->BindShader(m_hDivergence);
        pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
      }
      pContext->EndCompute();
      EndWrite(volume.m_hDivergence);
      EndWrite(volume.m_hPressure[0]);
      ++uiDispatches;

      const plUInt32 uiIterations = plMath::Clamp<plUInt32>(
        static_cast<plUInt32>(settings.m_uiPressureIterations * plMath::Clamp(cvar_FluidPressureIterationScale.GetValue(), 0.1f, 2.0f)), 1, 80);

      plUInt32 uiPressureRead = 0;
      for (plUInt32 i = 0; i < uiIterations; ++i)
      {
        const plUInt32 uiPressureWrite = 1 - uiPressureRead;

        BeginWrite(volume.m_hPressure[uiPressureWrite]);
        pContext->BeginCompute("Fluid Pressure");
        {
          plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
          bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
          bindGroup.BindTexture("FluidPressureInput", volume.m_hPressure[uiPressureRead]);
          bindGroup.BindTexture("FluidDivergenceInput", volume.m_hDivergence);
          bindGroup.BindTexture("FluidObstacleInput", volume.m_hObstacle);
          bindGroup.BindTexture("FluidPressureOutput", volume.m_hPressure[uiPressureWrite]);
          pContext->BindShader(m_hPressure);
          pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
        }
        pContext->EndCompute();
        EndWrite(volume.m_hPressure[uiPressureWrite]);

        uiPressureRead = uiPressureWrite;
        ++uiDispatches;
      }

      const plGALTextureHandle hStateRead = volume.m_hState[volume.m_uiCurrent];
      const plGALTextureHandle hStateWrite = volume.m_hState[1 - volume.m_uiCurrent];

      BeginWrite(hStateWrite);
      pContext->BeginCompute("Fluid Project");
      {
        plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
        bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
        bindGroup.BindTexture("FluidStateInput", hStateRead);
        bindGroup.BindTexture("FluidPressureInput", volume.m_hPressure[uiPressureRead]);
        bindGroup.BindTexture("FluidObstacleInput", volume.m_hObstacle);
        bindGroup.BindTexture("FluidStateOutput", hStateWrite);
        pContext->BindShader(m_hProject);
        pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
      }
      pContext->EndCompute();
      EndWrite(hStateWrite);

      volume.m_uiCurrent = 1 - volume.m_uiCurrent;
      ++uiDispatches;
    }
    else
    {
      StatePass("Fluid Snow", m_hSnow, false, false);
    }
  }

  // Snow: reduce the drift channel into the activity grid and start its readback to the CPU.
  StepActivity(volume, bRunSolver, pEncoder, pContext);

  // Water reads its live aux pair for the wet map; every other mode the zero placeholder, which Finalize ignores.
  const plGALTextureHandle hAux = volume.m_hAux[volume.m_uiAuxCurrent].IsInvalidated() ? GetAuxPlaceholder() : volume.m_hAux[volume.m_uiAuxCurrent];

  // Finalize always runs: the atlas rectangle may have moved even when the solver was idle.
  pContext->BeginCompute("Fluid Finalize");
  {
    plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
    bindGroup.BindTexture("FluidStateInput", volume.m_hState[volume.m_uiCurrent]);
    bindGroup.BindTexture("FluidAuxInput", hAux);
    bindGroup.BindTexture("FluidBedInput", hBed);
    bindGroup.BindTexture("FluidAtlasVelocityOutput", world.m_hAtlas[0]);
    bindGroup.BindTexture("FluidAtlasDensityOutput", world.m_hAtlas[1]);
    bindGroup.BindTexture("FluidAtlasSurfaceOutput", world.m_hAtlas[2]);
    pContext->BindShader(m_hFinalize);
    pContext->Dispatch(uiGroups, uiGroups, 1).IgnoreResult();
  }
  pContext->EndCompute();
  ++uiDispatches;

  // Smoke: the finished state extruded into the 3D texture the VDB volume renderer reads. Runs every frame like
  // Finalize and leaves the texture in ShaderRead. On the serial render path the fog passes read this frame's smoke;
  // on the threaded path this command list is submitted after the view graphs, so they read the previous frame's.
  // The texture is created with zeroed data, so the very first read is empty rather than undefined.
  if (!volume.m_hSmokeTexture.IsInvalidated())
  {
    const plUInt32 uiSmokeGroups = ThreadGroups(volume.m_uiSmokeRes);

    BeginWrite(volume.m_hSmokeTexture);
    pContext->BeginCompute("Fluid Smoke Extrude");
    {
      plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      bindGroup.BindBuffer("plFluidSolverConstants", volume.m_hConstants);
      bindGroup.BindTexture("FluidStateInput", volume.m_hState[volume.m_uiCurrent]);
      bindGroup.BindTexture("FluidSmokeOutput", volume.m_hSmokeTexture);
      pContext->BindShader(m_hSmokeExtrude);
      pContext->Dispatch(uiSmokeGroups, uiSmokeGroups, volume.m_uiSmokeDepth).IgnoreResult();
    }
    pContext->EndCompute();
    EndWrite(volume.m_hSmokeTexture);
    ++uiDispatches;
  }

  volume.m_Stats.m_uiDispatches = uiDispatches;
  volume.m_Stats.m_uiSteps = bRunSolver ? (bWater ? uiSubSteps : 1) : 0;
}

//////////////////////////////////////////////////////////////////////////
// atlas

plUInt32 plFluidGpuManager::Data::PackAtlas(plDynamicArray<Volume*>& ref_activeVolumes)
{
  if (ref_activeVolumes.IsEmpty())
    return 0;

  // Largest first. Every resolution is a power of two, so a shelf pack of a sorted list wastes nothing.
  ref_activeVolumes.Sort([](const Volume* pLhs, const Volume* pRhs) -> bool
    { return pLhs->m_Stats.m_uiResolution > pRhs->m_Stats.m_uiResolution; });

  const plUInt32 uiMaxSize = plMath::Clamp<plUInt32>(RoundUpPowerOfTwo(static_cast<plUInt32>(plMath::Max(cvar_FluidAtlasMaxSize.GetValue(), 256))), FluidMinAtlasSize, 8192);

  for (plUInt32 uiSize = plMath::Max(FluidMinAtlasSize, ref_activeVolumes[0]->m_Stats.m_uiResolution); uiSize <= uiMaxSize; uiSize *= 2)
  {
    plUInt32 uiX = 0;
    plUInt32 uiY = 0;
    plUInt32 uiShelfHeight = 0;
    bool bFits = true;

    for (Volume* pVolume : ref_activeVolumes)
    {
      const plUInt32 uiRes = pVolume->m_Stats.m_uiResolution;

      if (uiX + uiRes > uiSize)
      {
        uiX = 0;
        uiY += uiShelfHeight;
        uiShelfHeight = 0;
      }

      if (uiY + uiRes > uiSize)
      {
        bFits = false;
        break;
      }

      pVolume->m_vAtlasOffset.Set(uiX, uiY);
      uiX += uiRes;
      uiShelfHeight = plMath::Max(uiShelfHeight, uiRes);
    }

    if (bFits)
    {
      const float fInvSize = 1.0f / static_cast<float>(uiSize);
      for (Volume* pVolume : ref_activeVolumes)
      {
        const float fRes = static_cast<float>(pVolume->m_Stats.m_uiResolution);
        pVolume->m_AtlasRect.Set(pVolume->m_vAtlasOffset.x * fInvSize, pVolume->m_vAtlasOffset.y * fInvSize, fRes * fInvSize, fRes * fInvSize);
      }

      return uiSize;
    }
  }

  // Nothing fits at the cap: drop the smallest volumes until the rest do. The dropped ones keep their state and
  // come back as soon as there is room.
  while (ref_activeVolumes.GetCount() > 1)
  {
    ref_activeVolumes.PeekBack()->m_Stats.m_bActive = false;
    ref_activeVolumes.PopBack();

    const plUInt32 uiSize = PackAtlas(ref_activeVolumes);
    if (uiSize > 0)
      return uiSize;
  }

  return 0;
}

//////////////////////////////////////////////////////////////////////////
// per frame

void plFluidGpuManager::Data::UpdateWorld(plUInt32 uiWorldIndex, World& world)
{
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  const plUInt32 uiMaxActive = plMath::Clamp<plUInt32>(static_cast<plUInt32>(plMath::Max(cvar_FluidMaxActiveVolumes.GetValue(), 0)), 0, FLUID_MAX_VOLUMES);

  plHybridArray<Volume*, FLUID_MAX_VOLUMES> candidates;

  for (auto it = world.m_Volumes.GetIterator(); it.IsValid(); ++it)
  {
    Volume& volume = it.Value();
    volume.m_Stats = plFluidVolumeStats();

    // Readbacks are only ever mapped here, on the render side; a readback whose fence has not passed stays pending.
    ConsumeActivityReadback(volume);

    // A volume that stopped submitting (paused document, deactivated component) keeps its textures for a moment
    // and simply stops stepping.
    if (uiFrame - volume.m_uiSubmitFrame > 2 || !cvar_FluidEnable)
      continue;

    const plFluidSettings& settings = volume.m_Input.m_Settings;

    plUInt32 uiResolution = settings.GetClampedResolution();
    uiResolution = static_cast<plUInt32>(uiResolution * plMath::Clamp(cvar_FluidResolutionScale.GetValue(), 0.125f, 2.0f));

    // m_fLodFactor is the camera distance divided by the volume's LOD distance: halve the grid past 1, freeze past 2.
    if (volume.m_Input.m_fLodFactor > 2.0f)
      continue;

    if (volume.m_Input.m_fLodFactor > 1.0f)
      uiResolution /= 2;

    uiResolution = plMath::Clamp(RoundUpPowerOfTwo(uiResolution), FluidMinResolution, FluidMaxResolution);

    volume.m_Stats.m_uiResolution = uiResolution;
    volume.m_Stats.m_bActive = true;
    candidates.PushBack(&volume);
  }

  // Highest priority first, then nearest.
  candidates.Sort([](const Volume* pLhs, const Volume* pRhs) -> bool
    {
      if (pLhs->m_Input.m_iPriority != pRhs->m_Input.m_iPriority)
        return pLhs->m_Input.m_iPriority > pRhs->m_Input.m_iPriority;
      return pLhs->m_Input.m_fCameraDistance < pRhs->m_Input.m_fCameraDistance; });

  while (candidates.GetCount() > uiMaxActive)
  {
    candidates.PeekBack()->m_Stats.m_bActive = false;
    candidates.PopBack();
  }

  plDynamicArray<Volume*> active;
  active.Reserve(candidates.GetCount());
  for (Volume* pVolume : candidates)
    active.PushBack(pVolume);

  const plUInt32 uiAtlasSize = PackAtlas(active);

  EnsureWorldResources(world, uiAtlasSize);

  plFluidConstants* pWorldConstants = plRenderContext::GetConstantBufferData<plFluidConstants>(world.m_hConstants);
  pWorldConstants->FluidNumVolumes = active.GetCount();
  pWorldConstants->FluidDebugView = (static_cast<plUInt32>(plMath::Clamp<plInt32>(cvar_FluidDebugMaterial, 0, 255))) |
                                    (static_cast<plUInt32>(cvar_FluidDebugMaterialLayers.GetValue() & 0xFF) << 8);

  if (active.IsEmpty() || uiAtlasSize == 0)
  {
    pWorldConstants->FluidNumVolumes = 0;
    return;
  }

  pWorldConstants->FluidDeltaTime = static_cast<float>(active[0]->m_Input.m_DeltaTime.GetSeconds());
  pWorldConstants->FluidTime = static_cast<float>(plMath::Mod(plRenderWorld::GetFrameCounter() / 60.0, 1000.0));

  // Atlas or table just (re)created: their contents are undefined until this frame's command list runs, which on the
  // threaded path is after the views read them. Publishing no volumes makes every consumer skip the textures, and
  // undefined half floats can hold NaNs that TAA history would keep. The solver below still fills them for next frame.
  if (world.m_bFreshResources)
  {
    pWorldConstants->FluidNumVolumes = 0;
    world.m_bFreshResources = false;
  }

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plRenderContext* pRenderContext = plRenderContext::GetDefaultInstance();

  plGALCommandEncoder* pEncoder = pDevice->BeginCommands("Fluid Update");
  {
    for (plUInt32 i = 0; i < 3; ++i)
    {
      pEncoder->TextureBarrier(world.m_hAtlas[i], plGALResourceState::ShaderRead, plGALResourceState::UnorderedAccessWrite);
    }

    for (Volume* pVolume : active)
    {
      EnsureVolumeResources(*pVolume, pVolume->m_Stats.m_uiResolution, static_cast<plUInt8>(pVolume->m_Input.m_Settings.m_Mode.GetValue()), pVolume->m_Input.m_fInitialDepth, pEncoder, pRenderContext);
      EnsureSmokeResources(*pVolume, uiWorldIndex);
      StepVolume(*pVolume, world, pEncoder, pRenderContext);
    }

    for (plUInt32 i = 0; i < 3; ++i)
    {
      pEncoder->TextureBarrier(world.m_hAtlas[i], plGALResourceState::UnorderedAccessWrite, plGALResourceState::ShaderRead);
    }

    WriteTable(world, active.GetArrayPtr(), pEncoder);
  }
  pDevice->EndCommands(pEncoder);
}

void plFluidGpuManager::Data::WriteTable(World& world, plArrayPtr<Volume* const> activeVolumes, plGALCommandEncoder* pEncoder)
{
  plStaticArray<plVec4, FLUID_TABLE_ROWS * FLUID_MAX_VOLUMES> rows;
  rows.SetCount(FLUID_TABLE_ROWS * FLUID_MAX_VOLUMES);
  plMemoryUtils::ZeroFill(rows.GetData(), rows.GetCount());

  for (plUInt32 i = 0; i < activeVolumes.GetCount(); ++i)
  {
    const Volume& volume = *activeVolumes[i];
    const plFluidVolumeInput& input = volume.m_Input;

    plTransform transform = input.m_Transform;
    transform.m_vScale.Set(1.0f);

    const float fExtentX = plMath::Max(input.m_vExtents.x, 0.01f);
    const float fExtentY = plMath::Max(input.m_vExtents.y, 0.01f);

    // World to volume: rows 0 and 1 land in [0;1] across the rectangle, row 2 stays in metres from the plane.
    const plMat4 worldToLocal = transform.GetInverse().GetAsMat4();

    plVec4 row0 = worldToLocal.GetRow(0) * (1.0f / fExtentX);
    plVec4 row1 = worldToLocal.GetRow(1) * (1.0f / fExtentY);
    const plVec4 row2 = worldToLocal.GetRow(2);
    row0.w += 0.5f;
    row1.w += 0.5f;

    const plVec3 vAxisX = transform.m_qRotation * plVec3(1, 0, 0);
    const plVec3 vNormal = transform.m_qRotation * plVec3(0, 0, 1);

    const float fResolution = static_cast<float>(volume.m_Stats.m_uiResolution);
    const float fTexelSize = fExtentX / plMath::Max(fResolution, 1.0f);
    const float fEdgeFadeUv = plMath::Clamp(input.m_Settings.m_fEdgeFade / plMath::Min(fExtentX, fExtentY), 0.0001f, 0.5f);

    plVec4* pRow = rows.GetData() + i * FLUID_TABLE_ROWS;
    pRow[FLUID_ROW_WORLD_TO_VOLUME0] = row0;
    pRow[FLUID_ROW_WORLD_TO_VOLUME1] = row1;
    pRow[FLUID_ROW_WORLD_TO_VOLUME2] = row2;
    pRow[FLUID_ROW_ATLAS_RECT] = volume.m_AtlasRect;
    pRow[FLUID_ROW_PARAMS].Set(static_cast<float>(input.m_Settings.m_Mode.GetValue()), fEdgeFadeUv, input.m_Settings.m_fHeightScale, plMath::Max(input.m_fHalfThickness, 0.01f));
    pRow[FLUID_ROW_AXIS_X].Set(vAxisX.x, vAxisX.y, vAxisX.z, fTexelSize);
    pRow[FLUID_ROW_NORMAL].Set(vNormal.x, vNormal.y, vNormal.z, fResolution);
    pRow[FLUID_ROW_VORTEX].SetZero();
    pRow[FLUID_ROW_VORTEX_FLOW].SetZero();

    // Water materials map surface detail around the strongest vortex, so it is published with its centre on the plane.
    if (input.m_Settings.m_Mode == plFluidMode::Water)
    {
      const plFluidWorldSource* pStrongest = nullptr;
      for (const plFluidWorldSource& source : input.m_Sources)
      {
        if (source.m_uiShape == plFluidEmitterShape::Vortex && (pStrongest == nullptr || plMath::Abs(source.m_fSwirl) > plMath::Abs(pStrongest->m_fSwirl)))
          pStrongest = &source;
      }

      if (pStrongest != nullptr)
      {
        const plVec3 vOnPlane = pStrongest->m_vPosition - vNormal * vNormal.Dot(pStrongest->m_vPosition - transform.m_vPosition);
        pRow[FLUID_ROW_VORTEX].Set(vOnPlane.x, vOnPlane.y, vOnPlane.z, plMath::Max(pStrongest->m_fRadius, 0.01f));
        pRow[FLUID_ROW_VORTEX_FLOW].Set(pStrongest->m_fSwirl, pStrongest->m_fDrain, 0.0f, 0.0f);
      }
    }
  }

  plBoundingBoxu32 destBox;
  destBox.m_vMin.Set(0, 0, 0);
  destBox.m_vMax.Set(FLUID_TABLE_ROWS, FLUID_MAX_VOLUMES, 1);

  plGALSystemMemoryDescription memDesc;
  memDesc.m_uiRowPitch = FLUID_TABLE_ROWS * sizeof(plVec4);
  memDesc.m_pData = plMakeByteBlobPtr(rows.GetData(), memDesc.m_uiRowPitch * FLUID_MAX_VOLUMES);

  pEncoder->UpdateTexture(world.m_hTable, plGALTextureSubresource(), destBox, memDesc);
}

void plFluidGpuManager::OnRenderEvent(const plRenderWorldRenderEvent& e)
{
  if (e.m_Type != plRenderWorldRenderEvent::Type::BeginRender || s_pData == nullptr)
    return;

  PL_PROFILE_SCOPE("Fluid");
  PL_LOCK(s_pData->m_Mutex);

  s_pData->ReleaseRetiredReadbacks(false);

  plHybridArray<plUInt32, 4> emptyWorlds;
  plUInt32 uiActiveVolumes = 0;
  plUInt32 uiVramBytes = 0;

  for (auto it = s_pData->m_Worlds.GetIterator(); it.IsValid(); ++it)
  {
    Data::World& world = *it.Value();

    if (world.m_Volumes.GetCount() == 0)
    {
      if (++world.m_uiIdleFrames > 60)
        emptyWorlds.PushBack(it.Key());

      continue;
    }

    world.m_uiIdleFrames = 0;
    s_pData->UpdateWorld(it.Key(), world);

    for (auto volIt = world.m_Volumes.GetIterator(); volIt.IsValid(); ++volIt)
    {
      uiActiveVolumes += volIt.Value().m_Stats.m_bActive ? 1 : 0;
      uiVramBytes += volIt.Value().m_Stats.m_uiVramBytes;
    }

    if (world.m_uiAtlasSize > 0)
    {
      // velocity is RG16F, density and surface RGBA16F
      uiVramBytes += world.m_uiAtlasSize * world.m_uiAtlasSize * 20;
    }
  }

  for (plUInt32 uiWorld : emptyWorlds)
  {
    Data::World* pWorld = nullptr;
    if (s_pData->m_Worlds.Remove(uiWorld, &pWorld) && pWorld != nullptr)
    {
      s_pData->DestroyWorldResources(*pWorld);
      PL_DEFAULT_DELETE(pWorld);
    }
  }

  plStats::SetStat("Fluid/ActiveVolumes", uiActiveVolumes);
  plStats::SetStat("Fluid/VramMB", uiVramBytes / (1024 * 1024));
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Rendering_FluidGpuManager);
