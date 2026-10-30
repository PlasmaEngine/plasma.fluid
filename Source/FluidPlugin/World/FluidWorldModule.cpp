#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/GameApplication/GameApplicationBase.h>
#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Interfaces/WindWorldModule.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <Foundation/Utilities/Stats.h>
#include <RendererCore/Components/CameraComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Lights/WeatherComponent.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

#include <FluidPlugin/Components/FluidColliderComponent.h>
#include <FluidPlugin/Components/FluidEmitterComponent.h>
#include <FluidPlugin/Components/FluidVolumeComponent.h>
#include <FluidPlugin/World/FluidWorldModule.h>

plCVarBool cvar_FluidTouchPhysics("Fluid.Touch.Physics", true, plCVarFlags::Default, "Dynamic physics bodies and characters inside a fluid volume disturb it without a Fluid Emitter component");
plCVarInt cvar_FluidTouchMaxPhysicsSources("Fluid.Touch.MaxPhysicsSources", 16, plCVarFlags::Default, "Most physics bodies that disturb one fluid volume per frame, nearest to the volume centre first");
plCVarInt cvar_FluidTouchPhysicsCollisionLayer("Fluid.Touch.PhysicsCollisionLayer", -1, plCVarFlags::Default, "Collision layer of the physics touch query: bodies whose layer collides with it disturb the fluid. -1 queries with layer 0");
plCVarString cvar_FluidDebugScreenshots("Fluid.Debug.Screenshots", "", plCVarFlags::Default, "Comma separated world times in seconds at which the game application takes a screenshot. For automated captures of fluid scenes, e.g. -Fluid.Debug.Screenshots 5,15,30 on the Player's command line");
plCVarFloat cvar_FluidDebugQuitAfter("Fluid.Debug.QuitAfter", 0.0f, plCVarFlags::Default, "Fluid time in seconds after which the application quits; 0 never. For automated captures of fluid scenes");
plCVarBool cvar_FluidDebugLogStats("Fluid.Debug.LogStats", false, plCVarFlags::Default, "Logs every volume's mode, resolution, steps and state every two seconds of fluid time");
plCVarInt cvar_FluidDebugView("Fluid.Debug.View", -1, plCVarFlags::Default, "Overrides the Debug View of every volume: -1 uses each volume's own, 0 off, 1 velocity, 2 density, 4 height");
plCVarString cvar_FluidDebugCamera("Fluid.Debug.Camera", "", plCVarFlags::Default, "Pins the main view's camera every frame: '@px,py,pz,tx,ty,tz' looks from p at t. The @ keeps a leading minus from reading as a command line option. Several cameras separated by ';' are each captured by Fluid.Debug.Screenshots; the first is pinned otherwise");
plCVarFloat cvar_FluidDebugStepTime("Fluid.Debug.StepTime", 0.0f, plCVarFlags::Default, "Fluid time per frame in seconds regardless of the frame time; 0 uses the frame time. Reproduces slow or uneven frame rates, such as the editor's, at any frame rate");
plCVarString cvar_FluidDebugCaptureViews("Fluid.Debug.CaptureViews", "", plCVarFlags::Default, "Comma separated Fluid.Debug.Material views each Fluid.Debug.Screenshots time captures from every camera, e.g. 0,1,2");

extern plCVarInt cvar_FluidDebugMaterial;

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plFluidWorldModule);

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plFluidWorldModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  constexpr plUInt32 FluidBedResolution = 128;   ///< bed samples along each edge of a volume
  constexpr plUInt32 FluidBedRowsPerFrame = 16;  ///< raycast rows per volume and frame, so a bake never hitches
  constexpr float FluidBedRayMargin = 50.0f;     ///< metres the bake rays start above the slab and reach below it
  constexpr float FluidBedNoHitHeight = -100.0f; ///< bed height stored where a ray finds nothing: deep water
  constexpr float FluidBedSurfaceTolerance = 0.05f; ///< metres: hits closer than this to the water surface count as water

  /// True while vWorldPosition lies inside the box that boxTransform and vHalfExtents describe.
  bool IsInsideBox(const plTransform& boxTransform, const plVec3& vHalfExtents, const plVec3& vWorldPosition)
  {
    const plVec3 vLocal = boxTransform.GetInverse().TransformPosition(vWorldPosition);

    return plMath::Abs(vLocal.x) <= vHalfExtents.x && plMath::Abs(vLocal.y) <= vHalfExtents.y && plMath::Abs(vLocal.z) <= vHalfExtents.z;
  }
} // namespace

plFluidWorldModule::plFluidWorldModule(plWorld* pWorld)
  : plWorldModule(pWorld)
{
}

plFluidWorldModule::~plFluidWorldModule() = default;

void plFluidWorldModule::Initialize()
{
  SUPER::Initialize();

  // After the component updates moved this frame's emitters and colliders. The editor simulates fluid without Play,
  // so the update also runs while the world is not simulating.
  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plFluidWorldModule::UpdateVolumes, this);
  desc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostTransform;
  desc.m_bOnlyUpdateWhenSimulating = false;
  RegisterUpdateFunction(desc);
}

void plFluidWorldModule::Deinitialize()
{
  m_Volumes.Clear();
  m_Impulses.Clear();
  m_Bodies.Clear();
  m_BodyShapes.Clear();

  plFluidGpuManager::RemoveWorld(GetWorld()->GetIndex());

  SUPER::Deinitialize();
}

void plFluidWorldModule::RegisterVolume(plFluidVolumeComponent* pVolume)
{
  if (pVolume == nullptr)
    return;

  for (const VolumeEntry& entry : m_Volumes)
  {
    if (entry.m_pVolume == pVolume)
      return;
  }

  VolumeEntry& entry = m_Volumes.ExpandAndGetRef();
  entry.m_pVolume = pVolume;
}

void plFluidWorldModule::DeregisterVolume(plFluidVolumeComponent* pVolume)
{
  for (plUInt32 i = m_Volumes.GetCount(); i > 0; --i)
  {
    if (m_Volumes[i - 1].m_pVolume == pVolume)
    {
      m_Volumes.RemoveAtAndCopy(i - 1);
      return;
    }
  }
}

void plFluidWorldModule::AddImpulse(const plVec3& vPosition, float fRadius, const plVec3& vVelocity, float fAmount, float fHeightImpulse)
{
  plFluidWorldSource& source = m_Impulses.ExpandAndGetRef();
  source.m_vPosition = vPosition;
  source.m_vPrevPosition = vPosition;
  source.m_vVelocity = vVelocity;
  source.m_vExtents = plVec3(fRadius);
  source.m_fRadius = plMath::Max(fRadius, 0.01f);
  source.m_fDensity = fAmount;
  source.m_fTemperature = 0.0f;
  source.m_fHeightImpulse = fHeightImpulse;
  source.m_fPress = fAmount;
  source.m_fFalloff = 1.0f;
  source.m_uiShape = static_cast<plUInt8>(plFluidEmitterShape::Sphere);
}

bool plFluidWorldModule::PinDebugCamera(plUInt32 uiIndex)
{
  plStringBuilder sCameras = cvar_FluidDebugCamera.GetValue();
  sCameras.TrimWordStart("@");
  if (sCameras.IsEmpty())
    return false;

  plHybridArray<plStringView, 8> cameras;
  sCameras.Split(false, cameras, ";");
  if (uiIndex >= cameras.GetCount())
    return false;

  plStringBuilder sCamera = cameras[uiIndex];
  sCamera.TrimWordStart("@");

  plHybridArray<plStringView, 6> parts;
  sCamera.Split(false, parts, ",", " ");

  double values[6] = {};
  bool bValid = parts.GetCount() == 6;
  for (plUInt32 i = 0; bValid && i < 6; ++i)
    bValid = plConversionUtils::StringToFloat(parts[i], values[i]).Succeeded();

  plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld());
  if (!bValid || pView == nullptr || pView->GetCamera() == nullptr)
    return false;

  const plVec3 vPos(static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2]));
  const plVec3 vTarget(static_cast<float>(values[3]), static_cast<float>(values[4]), static_cast<float>(values[5]));
  pView->GetCamera()->LookAt(vPos, vTarget, plVec3::MakeAxisZ());
  return true;
}

void plFluidWorldModule::UpdateDebugCaptures()
{
  const double fNow = m_FluidTime.GetSeconds();

  PinDebugCamera(m_uiCaptureCamera);

  if (cvar_FluidDebugLogStats && fNow >= m_fNextStatsLog)
  {
    m_fNextStatsLog = fNow + 2.0;

    if (const plCameraComponentManager* pCameras = GetWorld()->GetComponentManager<plCameraComponentManager>())
    {
      for (auto it = pCameras->GetComponents(); it.IsValid(); ++it)
      {
        plLog::Info("Fluid: camera component on '{}' usage {} active {}", it->GetOwner()->GetName(), it->GetUsageHint().GetValue(), it->IsActive());
      }
    }

    if (const plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()))
    {
      if (const plCamera* pCamera = pView->GetCamera())
      {
        const plVec3 vPos = pCamera->GetCenterPosition();
        const plVec3 vDir = pCamera->GetCenterDirForwards();
        plLog::Info("Fluid: t={} s camera at ({}, {}, {}) looking ({}, {}, {})", plArgF(fNow, 1), plArgF(vPos.x, 1), plArgF(vPos.y, 1), plArgF(vPos.z, 1),
          plArgF(vDir.x, 2), plArgF(vDir.y, 2), plArgF(vDir.z, 2));
      }
    }

    for (const VolumeEntry& entry : m_Volumes)
    {
      if (entry.m_pVolume == nullptr || !entry.m_pVolume->IsActiveAndInitialized())
        continue;

      const plFluidVolumeStats stats = plFluidGpuManager::GetVolumeStats(GetWorld()->GetIndex(), entry.m_pVolume->GetVolumeHandle());
      plLog::Info("Fluid: t={} s '{}' mode {} res {} active {} asleep {} steps {} dispatches {}", plArgF(fNow, 1), entry.m_pVolume->GetOwner()->GetName(),
        entry.m_pVolume->GetEffectiveSettings().m_Mode.GetValue(), stats.m_uiResolution, stats.m_bActive, stats.m_bAsleep, stats.m_uiSteps, stats.m_uiDispatches);

      plFluidWaterStats water;
      if (plFluidGpuManager::GetWaterStats(GetWorld()->GetIndex(), entry.m_pVolume->GetVolumeHandle(), water))
      {
        plLog::Info("Fluid:   surface {} .. {} m, centre {} m, fastest flow {} m/s, invalid texels {}", plArgF(water.m_fLowestSurface, 2), plArgF(water.m_fHighestSurface, 2),
          plArgF(water.m_fCentreSurface, 2), plArgF(water.m_fFastestFlow, 2), water.m_fInvalidTexels);
      }
    }
  }

  plGameApplicationBase* pApp = plGameApplicationBase::GetGameApplicationBaseInstance();
  if (pApp == nullptr)
    return;

  // Material views of each shot; empty keeps whatever view is set.
  plHybridArray<plInt32, 16> views;
  {
    const plStringBuilder sViews = cvar_FluidDebugCaptureViews.GetValue();
    plHybridArray<plStringView, 16> parts;
    sViews.Split(false, parts, ",", " ");
    for (plStringView part : parts)
    {
      plInt32 iView = 0;
      if (plConversionUtils::StringToInt(part, iView).Succeeded())
        views.PushBack(iView);
    }
  }

  plUInt32 uiCameras = 1;
  {
    plStringBuilder sCameras = cvar_FluidDebugCamera.GetValue();
    plHybridArray<plStringView, 8> cameras;
    sCameras.Split(false, cameras, ";");
    uiCameras = plMath::Max(cameras.GetCount(), 1u);
  }
  const plUInt32 uiViews = plMath::Max(views.GetCount(), 1u);

  // A shot applies its camera and view, lets TAA and the solver settle, and captures twice, two frames apart. On DX12 a
  // capture can return the image of the capture before it (the first of a run comes back black), so only the second
  // capture of each shot is logged and used. A few more frames keep the next shot's camera out of the captured frame.
  constexpr plUInt32 CaptureSettleFrames = 16;
  constexpr plUInt32 CaptureSecondFrame = CaptureSettleFrames + 2;
  constexpr plUInt32 CaptureHoldFrames = 6;

  auto ApplyShot = [&](plInt32 iShot) {
    const plUInt32 uiShot = static_cast<plUInt32>(iShot);
    m_uiCaptureCamera = uiShot / uiViews;
    if (!views.IsEmpty())
      cvar_FluidDebugMaterial = views[uiShot % uiViews];
    m_uiCaptureFrame = 0;
    PinDebugCamera(m_uiCaptureCamera);
  };

  if (m_iCaptureShot < 0)
  {
    // The n-th listed time starts the n-th sequence, once.
    const plStringBuilder sTimes = cvar_FluidDebugScreenshots.GetValue();
    plHybridArray<plStringView, 8> times;
    sTimes.Split(false, times, ",", ";", " ");

    double fTime = 0.0;
    if (m_uiDebugScreenshotsTaken < times.GetCount() && plConversionUtils::StringToFloat(times[m_uiDebugScreenshotsTaken], fTime).Succeeded() && fNow >= fTime)
    {
      m_iCaptureRestoreView = cvar_FluidDebugMaterial;
      m_uiCaptureShots = uiCameras * uiViews;
      m_iCaptureShot = 0;
      ApplyShot(0);
    }
  }
  else
  {
    ++m_uiCaptureFrame;

    if (m_uiCaptureFrame == CaptureSettleFrames)
    {
      pApp->TakeScreenshot();
    }
    else if (m_uiCaptureFrame == CaptureSecondFrame)
    {
      plLog::Info("Fluid: shot {} camera {} view {} at fluid time {} s", m_uiDebugScreenshotsTaken, m_uiCaptureCamera, cvar_FluidDebugMaterial.GetValue(), plArgF(fNow, 2));
      pApp->TakeScreenshot();
    }
    else if (m_uiCaptureFrame >= CaptureSecondFrame + CaptureHoldFrames)
    {
      ++m_iCaptureShot;
      if (m_iCaptureShot < static_cast<plInt32>(m_uiCaptureShots))
      {
        ApplyShot(m_iCaptureShot);
      }
      else
      {
        m_iCaptureShot = -1;
        m_uiCaptureCamera = 0;
        cvar_FluidDebugMaterial = m_iCaptureRestoreView;
        ++m_uiDebugScreenshotsTaken;
      }
    }
  }

  // Never while a sequence still runs, so its last shot is captured.
  const float fQuitAfter = cvar_FluidDebugQuitAfter;
  if (fQuitAfter > 0.0f && fNow >= fQuitAfter && !m_bDebugQuitRequested && m_iCaptureShot < 0)
  {
    plLog::Info("Fluid: quitting at fluid time {} s", plArgF(fNow, 2));
    pApp->RequestQuit();
    m_bDebugQuitRequested = true;
  }
}

void plFluidWorldModule::UpdateVolumes(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);
  PL_PROFILE_SCOPE("Fluid.UpdateVolumes");

  // A world that is not simulating (the editor outside Play) holds its clock, so fluid runs on the global clock there
  // and keeps its own time, which is what lets the editor show a whirlpool turning while the scene is edited.
  plTime deltaTime = GetWorld()->GetWorldSimulationEnabled() ? GetWorld()->GetClock().GetTimeDiff() : plClock::GetGlobalClock()->GetTimeDiff();
  if (cvar_FluidDebugStepTime > 0.0f)
    deltaTime = plTime::MakeFromSeconds(cvar_FluidDebugStepTime);
  m_FluidTime += deltaTime;
  const plTime now = m_FluidTime;

  UpdateDebugCaptures();

  if (m_Volumes.IsEmpty())
  {
    m_Impulses.Clear();
    return;
  }

  plVec3 vCamera = plVec3::MakeZero();
  bool bHasCamera = false;
  if (const plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()))
  {
    if (const plCamera* pCamera = pView->GetCamera())
    {
      vCamera = pCamera->GetCenterPosition();
      bHasCamera = true;
    }
  }

  const plWindWorldModuleInterface* pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>();

  // The weather singleton is optional; without one every volume sees no precipitation.
  const plWeatherComponent* pWeather = nullptr;
  if (plWeatherComponentManager* pWeatherManager = GetWorld()->GetOrCreateComponentManager<plWeatherComponentManager>())
  {
    pWeather = pWeatherManager->GetSingletonComponent();
  }

  float fPrecipitation = 0.0f;
  if (pWeather != nullptr && pWeather->IsActiveAndInitialized())
  {
    fPrecipitation = plMath::Clamp(pWeather->GetRainIntensity() * pWeather->GetPrecipitationMultiplier(), 0.0f, 1.0f);
  }

  const plPhysicsWorldModuleInterface* pPhysics = GetWorld()->GetModuleReadOnly<plPhysicsWorldModuleInterface>();

  // Once per frame, not per volume, so every volume sees the same shapes and each body's history advances once.
  CollectBodies(static_cast<float>(deltaTime.GetSeconds()));

  plUInt32 uiSubmitted = 0;
  plUInt32 uiLandBakes = 0;

  for (VolumeEntry& entry : m_Volumes)
  {
    plFluidVolumeComponent* pVolume = entry.m_pVolume;
    if (pVolume == nullptr || !pVolume->IsActiveAndInitialized() || pVolume->GetVolumeHandle().IsInvalidated())
      continue;

    plFluidVolumeInput input;
    pVolume->BuildInput(input);
    input.m_DeltaTime = deltaTime;
    input.m_WorldTime = now;

    const plFluidSettings& settings = input.m_Settings;

    // Follow: the grid slides over the world on whole texels, so the simulated field does not swim under it. Only the
    // published transform moves; the owner game object is left where the scene put it.
    plVec3 vFollowTarget = plVec3::MakeZero();
    bool bFollow = false;
    if (pVolume->m_bFollowMainCamera && bHasCamera)
    {
      vFollowTarget = vCamera;
      bFollow = true;
    }
    else
    {
      plGameObject* pTarget = nullptr;
      if (GetWorld()->TryGetObject(pVolume->GetFollowTarget(), pTarget))
      {
        vFollowTarget = pTarget->GetGlobalPosition();
        bFollow = true;
      }
    }

    if (bFollow)
    {
      const plVec3 vRight = input.m_Transform.m_qRotation * plVec3::MakeAxisX();
      const plVec3 vUp = input.m_Transform.m_qRotation * plVec3::MakeAxisY();
      const plVec3 vNormal = input.m_Transform.m_qRotation * plVec3::MakeAxisZ();
      const plVec3 vDelta = vFollowTarget - input.m_Transform.m_vPosition;

      const float fTexelSize = input.m_vExtents.x / static_cast<float>(plMath::Max(settings.GetClampedResolution(), 1u));
      const float fSnappedX = plMath::Round(vDelta.Dot(vRight) / fTexelSize) * fTexelSize;
      const float fSnappedY = plMath::Round(vDelta.Dot(vUp) / fTexelSize) * fTexelSize;

      input.m_Transform.m_vPosition += vRight * fSnappedX + vUp * fSnappedY + vNormal * vDelta.Dot(vNormal);
    }

    input.m_fCameraDistance = bHasCamera ? (vCamera - input.m_Transform.m_vPosition).GetLength() : 0.0f;
    input.m_fLodFactor = bHasCamera ? input.m_fCameraDistance / plMath::Max(pVolume->m_fLodDistance, 0.01f) : 0.0f;

    const plVec3 vBoxHalfExtents(input.m_vExtents.x * 0.5f, input.m_vExtents.y * 0.5f, input.m_fHalfThickness);

    // Past twice the LOD distance the manager freezes the volume, so there is nothing to gather for. Between one
    // and two times it still simulates, at half the grid resolution.
    const bool bInRange = !bHasCamera || input.m_fLodFactor <= 2.0f;

    // Baked against the published transform, so a following volume rebakes as it slides.
    if (UpdateLandBake(entry, input, bInRange ? pPhysics : nullptr))
    {
      ++uiLandBakes;
    }

    if (bInRange)
    {
      // Order matters: the manager keeps the first sources up to its cap, so bodies come before the automatic
      // physics touch sources.
      GatherEmitters(input, input.m_Transform, vBoxHalfExtents);
      GatherImpulses(input, input.m_Transform, vBoxHalfExtents);

      plHybridArray<plFluidWorldCollider, 8> bodyColliders;
      GatherBodies(input, input.m_Transform, vBoxHalfExtents, bodyColliders);

      if (pVolume->m_bUseTouchSources && cvar_FluidTouchPhysics)
      {
        ++entry.m_uiPhysicsTouchFrame;
        GatherPhysicsTouchSources(entry, input, input.m_Transform, vBoxHalfExtents, pVolume->m_bTouchObstacles, bodyColliders);

        // Actors missing this frame restart their velocity when they come back.
        for (auto it = entry.m_PhysicsTouchStates.GetIterator(); it.IsValid();)
        {
          if (it.Value().m_uiLastSeenFrame != entry.m_uiPhysicsTouchFrame)
            it = entry.m_PhysicsTouchStates.Remove(it);
          else
            ++it;
        }
      }
      else if (!entry.m_PhysicsTouchStates.IsEmpty())
      {
        entry.m_PhysicsTouchStates.Clear();
      }

      GatherColliders(entry, input, input.m_Transform, vBoxHalfExtents);

      // Obstacles from Fluid Bodies and from physics bodies crossing the water plane move, so the mask is rebuilt
      // every frame they are present and once more after they leave.
      if (!bodyColliders.IsEmpty() || entry.m_bHadBodyObstacles)
      {
        input.m_bCollidersDirty = true;
      }
      entry.m_bHadBodyObstacles = !bodyColliders.IsEmpty();
      input.m_Colliders.PushBackRange(bodyColliders);

      if (pVolume->m_bUseWind && pWind != nullptr)
      {
        input.m_vWind = pWind->GetWindAt(input.m_Transform.m_vPosition) * settings.m_fWindStrength;
      }

      if (pVolume->m_bUseWeather)
      {
        // BuildInput already put the volume's own rate in. The weather component adds to it, so a volume can snow
        // on its own in a scene that has no weather at all.
        input.m_fPrecipitation = plMath::Clamp(input.m_fPrecipitation + fPrecipitation, 0.0f, 1.0f);
      }
    }
    else
    {
      // Keep the obstacle mask the GPU already holds. Colliders may move while the volume is out of range, so the
      // mask is rebuilt on the first gather after it comes back.
      input.m_bCollidersDirty = false;
      entry.m_bCollidersValid = false;
      entry.m_bHadBodyObstacles = false;
    }

    // Sleeping belongs to plFluidGpuManager: it sees the sources and the precipitation this frame and knows that
    // falling snow or rain is a source of its own, which a source-count test here would miss.
    plFluidGpuManager::SubmitVolume(GetWorld()->GetIndex(), pVolume->GetVolumeHandle(), std::move(input));
    ++uiSubmitted;
  }

  m_Impulses.Clear();

  for (const BodyEntry& body : m_Bodies)
  {
    body.m_pBody->AdvanceHistory(m_BodyShapes.GetArrayPtr().GetSubArray(body.m_uiFirstShape, body.m_uiShapeCount), body.m_Source);
  }

  DrawDebugView();

  plStats::SetStat("Fluid/Volumes", m_Volumes.GetCount());
  plStats::SetStat("Fluid/SubmittedVolumes", uiSubmitted);
  plStats::SetStat("Fluid/LandBakesInProgress", uiLandBakes);
}

bool plFluidWorldModule::UpdateLandBake(VolumeEntry& ref_entry, plFluidVolumeInput& ref_input, const plPhysicsWorldModuleInterface* pPhysics)
{
  LandBake& bake = ref_entry.m_LandBake;
  const plFluidVolumeComponent* pVolume = ref_entry.m_pVolume;

  // A volume that follows the camera or a target moves on almost every frame, and a bake needs several frames of a
  // still volume to finish. Its bed would never complete and the last one would sit in the wrong place, so a moving
  // volume gets no land: it is open water.
  const bool bFollows = pVolume->m_bFollowMainCamera || !pVolume->GetFollowTarget().IsInvalidated();

  if (!pVolume->m_bLandFromScene || bFollows)
  {
    bake = LandBake();
    ref_input.m_pBed = nullptr;
    return false;
  }

  plTransform transform = ref_input.m_Transform;
  transform.m_vScale.Set(1.0f);

  const bool bKeyChanged = !bake.m_bHasKey || !bake.m_Transform.m_vPosition.IsEqual(transform.m_vPosition, 0.01f) ||
                           !bake.m_Transform.m_qRotation.IsEqualRotation(transform.m_qRotation, 0.001f) ||
                           !bake.m_vExtents.IsEqual(ref_input.m_vExtents, 0.01f) ||
                           plMath::Abs(bake.m_fHalfThickness - ref_input.m_fHalfThickness) > 0.01f ||
                           bake.m_uiCollisionLayer != pVolume->m_uiLandCollisionLayer;

  // A restart keeps the published bed: the old ground is a better guess than none until the new one completes.
  if (bKeyChanged)
  {
    bake.m_Transform = transform;
    bake.m_vExtents = ref_input.m_vExtents;
    bake.m_fHalfThickness = ref_input.m_fHalfThickness;
    bake.m_uiCollisionLayer = pVolume->m_uiLandCollisionLayer;
    bake.m_bHasKey = true;

    bake.m_pPending = PL_DEFAULT_NEW(plFluidBed);
    bake.m_pPending->m_uiResolution = FluidBedResolution;
    bake.m_pPending->m_Heights.SetCount(FluidBedResolution * FluidBedResolution, FluidBedNoHitHeight);
    bake.m_uiNextRow = 0;
  }

  if (bake.m_pPending != nullptr && pPhysics != nullptr)
  {
    PL_PROFILE_SCOPE("Fluid Land Bake");

    const plVec3 vPlanePoint = bake.m_Transform.m_vPosition;
    const plVec3 vAxisX = bake.m_Transform.m_qRotation * plVec3::MakeAxisX();
    const plVec3 vAxisY = bake.m_Transform.m_qRotation * plVec3::MakeAxisY();
    const plVec3 vNormal = bake.m_Transform.m_qRotation * plVec3::MakeAxisZ();

    const float fExtentX = plMath::Max(bake.m_vExtents.x, 0.01f);
    const float fExtentY = plMath::Max(bake.m_vExtents.y, 0.01f);
    const float fStartHeight = plMath::Max(bake.m_fHalfThickness, 0.0f) + FluidBedRayMargin;
    const float fRayLength = 2.0f * fStartHeight;
    const float fInvRes = 1.0f / static_cast<float>(FluidBedResolution);

    // Terrain heightfields register as static bodies, so Static covers them together with static meshes.
    const plPhysicsQueryParameters params(bake.m_uiCollisionLayer, plPhysicsShapeType::Static);

    float* pHeights = bake.m_pPending->m_Heights.GetData();
    const plUInt32 uiEndRow = plMath::Min(bake.m_uiNextRow + FluidBedRowsPerFrame, FluidBedResolution);

    for (plUInt32 y = bake.m_uiNextRow; y < uiEndRow; ++y)
    {
      const float fLocalY = ((static_cast<float>(y) + 0.5f) * fInvRes - 0.5f) * fExtentY;

      for (plUInt32 x = 0; x < FluidBedResolution; ++x)
      {
        const float fLocalX = ((static_cast<float>(x) + 0.5f) * fInvRes - 0.5f) * fExtentX;
        const plVec3 vStart = vPlanePoint + vAxisX * fLocalX + vAxisY * fLocalY + vNormal * fStartHeight;

        float fHeight = FluidBedNoHitHeight;
        plPhysicsCastResult hit;
        if (pPhysics->Raycast(hit, vStart, -vNormal, fRayLength, params))
        {
          fHeight = (hit.m_vPosition - vPlanePoint).Dot(vNormal);

          // A hit right at the water surface is almost always the water's own collider (or a flat deck on it), not
          // ground: counted as land it would make the whole volume solid. It reads as the shallowest water instead.
          if (plMath::Abs(fHeight) < FluidBedSurfaceTolerance)
            fHeight = -FluidBedSurfaceTolerance;
        }

        pHeights[y * FluidBedResolution + x] = fHeight;
      }
    }

    bake.m_uiNextRow = uiEndRow;

    if (bake.m_uiNextRow >= FluidBedResolution)
    {
      bake.m_pPublished = bake.m_pPending;
      bake.m_pPending.Clear();
      bake.m_uiNextRow = 0;
    }
  }

  ref_input.m_pBed = bake.m_pPublished;
  return bake.m_pPending != nullptr;
}

void plFluidWorldModule::DrawDebugView()
{
  // The first volume with a Debug View other than Off decides what is shown. The whole atlas is drawn, not just that
  // volume's rectangle, because seeing every volume at once is what makes a missing or misplaced one obvious.
  plEnum<plFluidDebugView> view = plFluidDebugView::Off;
  for (const VolumeEntry& entry : m_Volumes)
  {
    const plFluidVolumeComponent* pVolume = entry.m_pVolume;
    if (pVolume != nullptr && pVolume->IsActiveAndInitialized() && pVolume->m_DebugView != plFluidDebugView::Off)
    {
      view = pVolume->m_DebugView;
      break;
    }
  }

  if (cvar_FluidDebugView >= 0)
  {
    view = static_cast<plFluidDebugView::Enum>(plMath::Clamp<plInt32>(cvar_FluidDebugView, 0, plFluidDebugView::Obstacles));
  }

  if (view == plFluidDebugView::Off)
    return;

  // Pressure and Obstacles live in the solver's private textures, which never reach the atlas.
  plUInt32 uiChannel = 1;
  switch (view)
  {
    case plFluidDebugView::Velocity:
      uiChannel = 0;
      break;
    case plFluidDebugView::Density:
      uiChannel = 1;
      break;
    case plFluidDebugView::Height:
      uiChannel = 2;
      break;
    default:
      return;
  }

  const plGALTextureHandle hAtlas = plFluidGpuManager::GetAtlasTexture(GetWorld()->GetIndex(), uiChannel);
  if (hAtlas.IsInvalidated())
    return;

  const plRectFloat rect(20.0f, 20.0f, 256.0f, 256.0f);
  plDebugRenderer::Draw2DRectangle(GetWorld(), rect, 0.0f, plColor::White, hAtlas);
}

void plFluidWorldModule::GatherEmitters(plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents)
{
  plFluidEmitterComponentManager* pManager = GetWorld()->GetComponentManager<plFluidEmitterComponentManager>();
  if (pManager == nullptr)
    return;

  for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
  {
    plFluidEmitterComponent& emitter = *it;
    if (!emitter.IsActiveAndInitialized())
      continue;

    if (!IsInsideBox(boxTransform, vBoxHalfExtents, emitter.GetOwner()->GetGlobalPosition()))
      continue;

    plFluidWorldSource source;
    if (!emitter.FillSource(source, m_FluidTime))
      continue;

    ref_input.m_Sources.PushBack(source);
    emitter.AdvanceTrail();
  }
}

void plFluidWorldModule::GatherImpulses(plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents) const
{
  for (const plFluidWorldSource& impulse : m_Impulses)
  {
    if (IsInsideBox(boxTransform, vBoxHalfExtents, impulse.m_vPosition))
    {
      ref_input.m_Sources.PushBack(impulse);
    }
  }
}

void plFluidWorldModule::GatherPhysicsTouchSources(VolumeEntry& ref_entry, plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents, bool bObstacles, plDynamicArray<plFluidWorldCollider>& out_colliders)
{
  const plPhysicsWorldModuleInterface* pPhysics = GetWorld()->GetModuleReadOnly<plPhysicsWorldModuleInterface>();
  if (pPhysics == nullptr)
    return;

  const plUInt32 uiMaxSources = static_cast<plUInt32>(plMath::Max(cvar_FluidTouchMaxPhysicsSources.GetValue(), 0));
  if (uiMaxSources == 0)
    return;

  PL_PROFILE_SCOPE("Fluid Physics Touch");

  const plInt32 iLayer = cvar_FluidTouchPhysicsCollisionLayer.GetValue();
  const plUInt32 uiLayer = iLayer < 0 ? 0u : static_cast<plUInt32>(plMath::Min(iLayer, 31));
  const plBitflags<plPhysicsShapeType> shapeTypes = plPhysicsShapeType::Dynamic | plPhysicsShapeType::Character;

  plPhysicsQueryParameters params(uiLayer, shapeTypes);

  plPhysicsOverlapResultArray results;
  pPhysics->QueryShapesInBox(results, vBoxHalfExtents * 2.0f, boxTransform, params);

  if (results.m_Results.IsEmpty())
    return;

  struct Candidate
  {
    plVec3 m_vPosition;
    plVec3 m_vVelocity;
    float m_fRadius;
    float m_fDistanceSqr;
    float m_fBoundsRadius; ///< bounding sphere radius, 0 when the actor has no bounds
  };

  plHybridArray<Candidate, 16> candidates;
  const float fDeltaTime = static_cast<float>(GetWorld()->GetClock().GetTimeDiff().GetSeconds());

  for (const plPhysicsOverlapResult& hit : results.m_Results)
  {
    const plGameObjectHandle hActor = !hit.m_hActorObject.IsInvalidated() ? hit.m_hActorObject : hit.m_hShapeObject;

    plGameObject* pActor = nullptr;
    if (!GetWorld()->TryGetObject(hActor, pActor))
      continue;

    // One body yields one result per touched sub-shape.
    // States of actors missing last frame were pruned, so an existing state holds last frame's position.
    PhysicsTouchState* pState = nullptr;
    const bool bHasLast = ref_entry.m_PhysicsTouchStates.TryGetValue(hActor, pState);
    if (bHasLast && pState->m_uiLastSeenFrame == ref_entry.m_uiPhysicsTouchFrame)
      continue;

    // Bodies carrying their own emitter already speak for themselves.
    const plFluidEmitterComponent* pEmitter = nullptr;
    if (pActor->TryGetComponentOfBaseType(pEmitter) && pEmitter->IsActiveAndInitialized())
      continue;

    // So do actors under a Fluid Body, such as ragdoll limbs on child objects of a character.
    if (HasFluidBodyInHierarchy(pActor))
      continue;

    // Character controllers have no actor bounds; their object bounds or a default sphere at the origin stand in.
    plBoundingBoxSphere bounds = pPhysics->GetWorldSpaceBounds(pActor, uiLayer, shapeTypes, false);
    if (!bounds.IsValid())
      bounds = pActor->GetGlobalBounds();

    Candidate candidate;
    if (bounds.IsValid())
    {
      const plBoundingBox box = bounds.GetBox();
      const plVec3 vHalf = box.GetHalfExtents();
      candidate.m_vPosition = box.GetCenter();
      candidate.m_fRadius = plMath::Clamp(plMath::Max(vHalf.x, vHalf.y), 0.05f, 8.0f);
      candidate.m_fBoundsRadius = plMath::Min(bounds.m_fSphereRadius, vHalf.GetLength());
    }
    else
    {
      candidate.m_vPosition = pActor->GetGlobalPosition();
      candidate.m_fRadius = 0.5f;
      candidate.m_fBoundsRadius = 0.0f;
    }

    if (!IsInsideBox(boxTransform, vBoxHalfExtents, candidate.m_vPosition))
      continue;

    PhysicsTouchState& state = ref_entry.m_PhysicsTouchStates[hActor];
    candidate.m_vVelocity = (bHasLast && fDeltaTime > 0.0001f) ? (candidate.m_vPosition - state.m_vLastPosition) / fDeltaTime : plVec3::MakeZero();
    state.m_vLastPosition = candidate.m_vPosition;
    state.m_uiLastSeenFrame = ref_entry.m_uiPhysicsTouchFrame;

    candidate.m_fDistanceSqr = (candidate.m_vPosition - boxTransform.m_vPosition).GetLengthSquared();
    candidates.PushBack(candidate);
  }

  if (candidates.GetCount() > uiMaxSources)
  {
    candidates.Sort([](const Candidate& a, const Candidate& b)
      { return a.m_fDistanceSqr < b.m_fDistanceSqr; });
    candidates.SetCount(uiMaxSources);
  }

  const plVec3 vNormal = boxTransform.m_qRotation * plVec3::MakeAxisZ();
  const float fTouchStrength = ref_input.m_Settings.m_fTouchStrength;
  const float fRadiusScale = plMath::Max(ref_input.m_Settings.m_fTouchRadiusScale, 0.01f);

  for (const Candidate& candidate : candidates)
  {
    plFluidWorldSource& source = ref_input.m_Sources.ExpandAndGetRef();
    source.m_vPosition = candidate.m_vPosition;
    source.m_vPrevPosition = candidate.m_vPosition - candidate.m_vVelocity * plMath::Max(fDeltaTime, 0.0f);
    source.m_vVelocity = candidate.m_vVelocity * fTouchStrength;
    source.m_fRadius = candidate.m_fRadius * fRadiusScale;
    source.m_vExtents = plVec3(source.m_fRadius);
    source.m_fDensity = 0.0f;
    source.m_fTemperature = 0.0f;
    // Only downward motion dents a height field; the amount follows how fast the body comes down.
    source.m_fHeightImpulse = plMath::Min(candidate.m_vVelocity.Dot(vNormal), 0.0f) * fTouchStrength;
    source.m_fPress = 1.0f;
    source.m_fFalloff = 1.0f;
    source.m_uiShape = static_cast<plUInt8>(plFluidEmitterShape::Sphere);

    if (!bObstacles || candidate.m_fBoundsRadius <= 0.0f)
      continue;

    // A body whose bounds straddle the water plane is also an obstacle: a sphere at the waterline with the bounds'
    // cross-section radius there, no wider than the body's footprint in the plane.
    const float fDistance = (candidate.m_vPosition - boxTransform.m_vPosition).Dot(vNormal);
    const float fBoundsRadius = candidate.m_fBoundsRadius;
    if (plMath::Abs(fDistance) >= fBoundsRadius)
      continue;

    const float fCrossSection = plMath::Min(plMath::Sqrt(fBoundsRadius * fBoundsRadius - fDistance * fDistance), candidate.m_fRadius);

    plFluidWorldCollider& collider = out_colliders.ExpandAndGetRef();
    collider.m_vPosition = candidate.m_vPosition - vNormal * fDistance;
    collider.m_vExtents = plVec3(fCrossSection);
    collider.m_fRotation = 0.0f;
    collider.m_fSlip = 0.0f;
    collider.m_qRotation = plQuat::MakeIdentity();
    collider.m_uiShape = static_cast<plUInt8>(plFluidColliderShape::Sphere);
  }
}

void plFluidWorldModule::GatherColliders(VolumeEntry& ref_entry, plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents)
{
  plFluidColliderComponentManager* pManager = GetWorld()->GetComponentManager<plFluidColliderComponentManager>();

  plHybridArray<plTransform, 8> transforms;
  bool bDirty = !ref_entry.m_bCollidersValid;

  if (pManager != nullptr)
  {
    for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
    {
      const plFluidColliderComponent& collider = *it;
      if (!collider.IsActiveAndInitialized())
        continue;

      const plTransform transform = collider.GetGlobalTransform();
      if (!IsInsideBox(boxTransform, vBoxHalfExtents, transform.m_vPosition))
        continue;

      plFluidWorldCollider worldCollider;
      if (!collider.FillCollider(worldCollider))
        continue;

      const plUInt32 uiIndex = transforms.GetCount();
      transforms.PushBack(transform);
      ref_input.m_Colliders.PushBack(worldCollider);

      // A collider marked static never forces a rebuild, even if something moved it.
      if (!collider.m_bStatic && uiIndex < ref_entry.m_LastColliders.GetCount() && collider.HasMovedSince(ref_entry.m_LastColliders[uiIndex]))
      {
        bDirty = true;
      }
    }
  }

  // A different set means the gather order shifted, so the remembered transforms no longer line up.
  if (transforms.GetCount() != ref_entry.m_LastColliders.GetCount())
  {
    bDirty = true;
  }

  ref_entry.m_LastColliders = transforms;
  ref_entry.m_bCollidersValid = true;
  ref_input.m_bCollidersDirty = bDirty;
}

void plFluidWorldModule::CollectBodies(float fDeltaTime)
{
  m_Bodies.Clear();
  m_BodyShapes.Clear();

  plFluidBodyComponentManager* pManager = GetWorld()->GetComponentManager<plFluidBodyComponentManager>();
  if (pManager == nullptr)
    return;

  for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
  {
    plFluidBodyComponent& body = *it;
    if (!body.IsActiveAndInitialized())
      continue;

    BodyEntry& entry = m_Bodies.ExpandAndGetRef();
    entry.m_pBody = &body;
    entry.m_uiFirstShape = m_BodyShapes.GetCount();
    entry.m_Source = body.ComputeShapes(m_BodyShapes, fDeltaTime);
    entry.m_uiShapeCount = m_BodyShapes.GetCount() - entry.m_uiFirstShape;
  }
}

void plFluidWorldModule::GatherBodies(plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents, plDynamicArray<plFluidWorldCollider>& out_colliders) const
{
  if (m_Bodies.IsEmpty())
    return;

  const plVec3 vNormal = boxTransform.m_qRotation * plVec3::MakeAxisZ();

  for (const BodyEntry& body : m_Bodies)
  {
    const plFluidBodyComponent* pBody = body.m_pBody;

    for (plUInt32 i = 0; i < body.m_uiShapeCount; ++i)
    {
      const plFluidBodyComponent::Shape& shape = m_BodyShapes[body.m_uiFirstShape + i];

      float fReach = shape.m_fRadius;
      if (shape.m_uiShape == plFluidEmitterShape::Capsule)
        fReach += shape.m_vAxis.GetLength();
      else if (shape.m_uiShape == plFluidEmitterShape::Box)
        fReach += shape.m_vHalfExtents.GetLength();

      if (!IsInsideBox(boxTransform, vBoxHalfExtents + plVec3(fReach), shape.m_vCenter))
        continue;

      // Moving through the surface or sinking into it pushes water down.
      const float fAlongNormal = shape.m_vVelocity.Dot(vNormal);
      const float fInPlaneSpeed = (shape.m_vVelocity - vNormal * fAlongNormal).GetLength();

      plFluidWorldSource& source = ref_input.m_Sources.ExpandAndGetRef();
      source.m_vPosition = shape.m_vCenter;
      source.m_vPrevPosition = shape.m_vLastCenter;
      source.m_vVelocity = shape.m_vVelocity * pBody->m_fStrength;
      source.m_vExtents = shape.m_vHalfExtents;
      source.m_vAxis = shape.m_vAxis;
      source.m_fRadius = shape.m_fRadius;
      source.m_fDensity = 0.0f;
      source.m_fTemperature = 0.0f;
      source.m_fHeightImpulse = -pBody->m_fWake * (0.5f * fInPlaneSpeed + plMath::Max(-fAlongNormal, 0.0f));
      source.m_fPress = pBody->m_fPress;
      source.m_fFalloff = 1.0f;
      source.m_uiShape = shape.m_uiShape;

      if (!pBody->m_bObstacle)
        continue;

      // Packed the way the obstacle kernel reads colliders: Sphere extents.x is the radius, Box extents are half
      // extents in the rotation's frame, Capsule extents are (half length in the plane, radius) along the rotation's
      // local X.
      plFluidWorldCollider& collider = out_colliders.ExpandAndGetRef();
      collider.m_vPosition = shape.m_vCenter;
      collider.m_fRotation = 0.0f;
      collider.m_fSlip = 0.0f;

      if (shape.m_uiShape == plFluidEmitterShape::Capsule)
      {
        plVec3 vInPlane = shape.m_vAxis - vNormal * shape.m_vAxis.Dot(vNormal);
        const float fHalfLength = vInPlane.GetLength();

        collider.m_qRotation = plQuat::MakeIdentity();
        if (fHalfLength > 0.0001f)
        {
          vInPlane /= fHalfLength;
          collider.m_qRotation = plQuat::MakeShortestRotation(plVec3::MakeAxisX(), vInPlane);
        }

        collider.m_vExtents.Set(fHalfLength, shape.m_fRadius, shape.m_fRadius);
        collider.m_uiShape = static_cast<plUInt8>(plFluidColliderShape::Capsule);
      }
      else if (shape.m_uiShape == plFluidEmitterShape::Box)
      {
        collider.m_qRotation = shape.m_qRotation;
        collider.m_vExtents = shape.m_vHalfExtents;
        collider.m_uiShape = static_cast<plUInt8>(plFluidColliderShape::Box);
      }
      else
      {
        collider.m_qRotation = plQuat::MakeIdentity();
        collider.m_vExtents = plVec3(shape.m_fRadius);
        collider.m_uiShape = static_cast<plUInt8>(plFluidColliderShape::Sphere);
      }
    }
  }
}

bool plFluidWorldModule::HasFluidBodyInHierarchy(const plGameObject* pObject)
{
  for (; pObject != nullptr; pObject = pObject->GetParent())
  {
    const plFluidBodyComponent* pBody = nullptr;
    if (pObject->TryGetComponentOfBaseType(pBody) && pBody->IsActiveAndInitialized())
      return true;
  }

  return false;
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_World_FluidWorldModule);
