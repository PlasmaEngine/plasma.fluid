#include <FluidTest/FluidTestPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <FluidPlugin/Components/FluidEmitterComponent.h>
#include <FluidPlugin/Components/FluidVolumeComponent.h>
#include <FluidPlugin/World/FluidWorldModule.h>
#include <FluidTest/FluidCloudsTest.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/Time/Clock.h>
#include <Foundation/Types/TagRegistry.h>
#include <RendererCore/Lights/DirectionalLightComponent.h>
#include <RendererCore/Lights/SkyAtmosphereComponent.h>
#include <RendererCore/Lights/SkyLightComponent.h>
#include <RendererCore/Lights/VolumetricCloudsComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

static plGameEngineTestFluidClouds s_GameEngineTestFluidClouds;

namespace
{
  const plVec3 s_vUp(0.0f, 0.0f, 1.0f);

  template <typename CVAR, typename VALUE>
  void SetCVar(const char* szName, VALUE value)
  {
    if (auto* pCVar = static_cast<CVAR*>(plCVar::FindCVarByName(szName)))
      *pCVar = value;
    else
      PL_TEST_FAILURE("CVar not found", "%s", szName);
  }

  /// A deterministic pseudo random number in [0, 1) per index and salt.
  float Hash01(plUInt32 i, plUInt32 uiSalt)
  {
    plUInt32 x = i * 747796405u + uiSalt * 2891336453u + 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
  }

  /// PL_FLUIDCLOUDS_CVARS="Name=value;Name=value" sets any cvar for a run, for A/B comparisons without a rebuild.
  void ApplyCVarOverrides()
  {
    plStringBuilder sAll = plEnvironmentVariableUtils::GetValueString("PL_FLUIDCLOUDS_CVARS");
    plDynamicArray<plStringView> entries;
    sAll.Split(false, entries, ";");
    for (plStringView entry : entries)
    {
      const char* szEquals = entry.FindSubString("=");
      if (szEquals == nullptr)
        continue;
      plStringBuilder sName(plStringView(entry.GetStartPointer(), szEquals));
      plStringBuilder sValue(plStringView(szEquals + 1, entry.GetEndPointer()));
      plCVar* pCVar = plCVar::FindCVarByName(sName);
      if (pCVar == nullptr)
      {
        PL_TEST_FAILURE("CVar not found", "%s", sName.GetData());
        continue;
      }
      switch (pCVar->GetType())
      {
        case plCVarType::Bool:
          *static_cast<plCVarBool*>(pCVar) = sValue == "1" || sValue.IsEqual_NoCase("true");
          break;
        case plCVarType::Int:
          *static_cast<plCVarInt*>(pCVar) = atoi(sValue.GetData());
          break;
        case plCVarType::Float:
          *static_cast<plCVarFloat*>(pCVar) = static_cast<float>(atof(sValue.GetData()));
          break;
        default:
          *static_cast<plCVarString*>(pCVar) = sValue.GetData();
          break;
      }
    }
  }

  float SmoothStep01(float x)
  {
    x = plMath::Clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Game state and the package directories

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plFluidTestGameState, 1, plRTTIDefaultAllocator<plFluidTestGameState>)
PL_END_DYNAMIC_REFLECTED_TYPE;

static plString s_sSdkBinaryDirectory;
static plString s_sPackagePluginDirectory;
static plString s_sPackageAssetsDirectory;

plFluidTestApplication::plFluidTestApplication(const char* szProjectDirName)
  : plGameEngineTestApplication(szProjectDirName)
{
}

void plFluidTestApplication::SetPackageDirectories(plStringView sSdkBinaryDirectory, plStringView sPluginDirectory, plStringView sAssetsDirectory)
{
  s_sSdkBinaryDirectory = sSdkBinaryDirectory;
  s_sPackagePluginDirectory = sPluginDirectory;
  s_sPackageAssetsDirectory = sAssetsDirectory;
}

void plFluidTestApplication::Init_AddPluginSearchRoots()
{
  plGameEngineTestApplication::Init_AddPluginSearchRoots();

  if (!s_sSdkBinaryDirectory.IsEmpty())
    plPlugin::AddPluginSearchRoot(s_sSdkBinaryDirectory);
  if (!s_sPackagePluginDirectory.IsEmpty())
    plPlugin::AddPluginSearchRoot(s_sPackagePluginDirectory);
}

void plFluidTestApplication::Init_FileSystem_ConfigureDataDirs()
{
  plGameEngineTestApplication::Init_FileSystem_ConfigureDataDirs();

  if (!s_sPackageAssetsDirectory.IsEmpty())
    plFileSystem::AddDataDirectory(s_sPackageAssetsDirectory, "FluidTest", "plasma.fluid", plDataDirUsage::ReadOnly).AssertSuccess();
}

//////////////////////////////////////////////////////////////////////////
// Clips

plFluidCloudsApplication::plFluidCloudsApplication()
  : plFluidTestApplication("FluidClouds")
{
}

plUniquePtr<plGameStateBase> plFluidCloudsApplication::CreateGameState()
{
  return PL_DEFAULT_NEW(plFluidTestGameState);
}

void plFluidCloudsApplication::BeforeHighLevelSystemsShutdown()
{
  m_hBoxMesh.Invalidate();
  m_hGlowBoxMesh.Invalidate();
  plFluidTestApplication::BeforeHighLevelSystemsShutdown();
}

const plFluidCloudsApplication::ClipInfo& plFluidCloudsApplication::GetClipInfo(Clip clip)
{
  // name, title, frames, warm-up frames, warm-up step, step. The hurricane spins up for six simulated minutes first.
  static const ClipInfo s_Clips[(int)Clip::Count] = {
    {"HurricaneOrbit", "Hurricane - plasma.fluid Gas weather map driving the volumetric clouds", 600, 500, 12.0f, 2.0f},
    {"HurricaneEye", "Hurricane - inside the eye", 450, 500, 12.0f, 2.0f},
    {"BeamGround", "Beam parting the clouds - from the ground", 450, 40, 0.15f, 0.15f},
    {"BeamAerial", "Beam parting the clouds - from above", 450, 40, 0.15f, 0.15f},
  };
  return s_Clips[(int)clip];
}

void plFluidCloudsApplication::AddSunAndSky(float fLux, bool bShadows)
{
  plGameObjectDesc sunDesc;
  sunDesc.m_sName.Assign("Sun");
  sunDesc.m_bDynamic = true; // the clips move it
  plGameObject* pSunObject = nullptr;
  m_hSun = m_pWorld->CreateObject(sunDesc, pSunObject);
  plDirectionalLightComponent* pSun = nullptr;
  plDirectionalLightComponent::CreateComponent(pSunObject, pSun);
  pSun->SetIntensity(fLux);
  pSun->SetCastShadows(bShadows);

  plGameObjectDesc skyDesc;
  skyDesc.m_sName.Assign("Sky");
  plGameObject* pSkyObject = nullptr;
  m_pWorld->CreateObject(skyDesc, pSkyObject);
  plSkyAtmosphereComponent* pAtmosphere = nullptr;
  plSkyAtmosphereComponent::CreateComponent(pSkyObject, pAtmosphere);
  plSkyLightComponent* pSkyLight = nullptr;
  plSkyLightComponent::CreateComponent(pSkyObject, pSkyLight);
}

void plFluidCloudsApplication::SetSun(float fElevationDegrees, float fAzimuthDegrees)
{
  const plAngle elevation = plAngle::MakeFromDegree(fElevationDegrees);
  const plAngle azimuth = plAngle::MakeFromDegree(fAzimuthDegrees);
  const plVec3 vToSun(plMath::Cos(elevation) * plMath::Cos(azimuth), plMath::Cos(elevation) * plMath::Sin(azimuth), plMath::Sin(elevation));

  plGameObject* pSun = nullptr;
  if (m_pWorld->TryGetObject(m_hSun, pSun))
    pSun->SetLocalRotation(plQuat::MakeShortestRotation(plVec3(-1.0f, 0.0f, 0.0f), vToSun.GetNormalized()));
}

void plFluidCloudsApplication::SetCamera(const plVec3& vPosIn, const plVec3& vTargetIn, float fFovYIn)
{
  plVec3 vPos = vPosIn;
  plVec3 vTarget = vTargetIn;
  float fFovY = fFovYIn;

  // PL_FLUIDCLOUDS_TOPDOWN=<metres across>: straight down from far above, for looking at the weather map itself
  // (with Rendering.VolumetricClouds.Debug=1 the clouds show their coverage).
  const plInt32 iTopDown = plEnvironmentVariableUtils::GetValueInt("PL_FLUIDCLOUDS_TOPDOWN", 0);
  if (iTopDown > 0)
  {
    vPos.Set(0.0f, 0.0f, 150000.0f);
    vTarget.Set(0.0f, 1.0f, 0.0f);
    fFovY = 2.0f * plMath::ATan(0.5f * iTopDown / 150000.0f).GetDegree();
  }

  if (auto* pGameState = plDynamicCast<plFluidTestGameState*>(GetActiveGameState()))
  {
    // Kilometres up, nothing is near: a far near plane keeps the depth buffer precise out to the horizon.
    // Kilometres up nothing is near: a far near plane keeps the depth buffer precise out to the horizon.
    float fNear = plMath::Clamp(vPos.z * 0.002f, 0.5f, 100.0f);
    const plInt32 iNearOverride = plEnvironmentVariableUtils::GetValueInt("PL_FLUIDCLOUDS_NEAR", 0);
    if (iNearOverride > 0)
      fNear = static_cast<float>(iNearOverride);
    pGameState->GetMainCamera()->SetCameraMode(plCameraMode::PerspectiveFixedFovY, fFovY, fNear, 400000.0f);
    pGameState->GetMainCamera()->LookAt(vPos, vTarget, s_vUp);
  }
}

plGameObject* plFluidCloudsApplication::AddBox(const char* szName, const plVec3& vCenter, const plVec3& vSize, const plColor& color, bool bFullbright, bool bCastShadow)
{
  plMeshResourceHandle& hMesh = bFullbright ? m_hGlowBoxMesh : m_hBoxMesh;
  if (!hMesh.IsValid())
  {
    plGeometry geometry;
    geometry.AddBox(plVec3(1.0f), true);
    geometry.ComputeTangents();

    plMeshResourceDescriptor descriptor;
    plMeshBufferResourceDescriptor& meshBuffer = descriptor.MeshBufferDesc();
    meshBuffer.AddCommonStreams();
    meshBuffer.AllocateStreamsFromGeometry(geometry, plGALPrimitiveTopology::Triangles);
    descriptor.AddSubMesh(meshBuffer.GetPrimitiveCount(), 0, 0);
    // Fullbright shows the mesh colour unlit, so an HDR colour glows.
    descriptor.SetMaterial(0, bFullbright ? "{ 05af8d07-0b38-44a6-8d50-49731ae2625d }" : "{ 7c71fc2d-2a1a-4938-9305-f3a0f26d73cc }");
    descriptor.ComputeBounds();
    hMesh = plResourceManager::GetOrCreateResource<plMeshResource>(bFullbright ? "GameEngineTest/FluidClouds/GlowBox" : "GameEngineTest/FluidClouds/Box", std::move(descriptor), "Fluid clouds box");
  }

  plGameObjectDesc desc;
  desc.m_sName.Assign(szName);
  desc.m_LocalPosition = vCenter;
  desc.m_LocalScaling = vSize;
  desc.m_bDynamic = bFullbright;
  if (!bFullbright && bCastShadow)
    desc.m_Tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
  plGameObject* pObject = nullptr;
  m_pWorld->CreateObject(desc, pObject);

  plMeshComponent* pMesh = nullptr;
  m_pWorld->GetOrCreateComponentManager<plMeshComponentManager>()->CreateComponent(pObject, pMesh);
  pMesh->SetMesh(hMesh);
  pMesh->SetColor(color);
  return pObject;
}

plVolumetricCloudsComponent* plFluidCloudsApplication::AddClouds(const WeatherSetup& weather)
{
  plGameObjectDesc desc;
  desc.m_sName.Assign("Clouds");
  plGameObject* pObject = nullptr;
  m_pWorld->CreateObject(desc, pObject);

  plVolumetricCloudsComponent* pClouds = nullptr;
  plVolumetricCloudsComponent::CreateComponent(pObject, pClouds);
  pClouds->SetHeightRangeMin(weather.m_fLayerMin);
  pClouds->SetHeightRangeMax(weather.m_fLayerMax);
  // Only the fluid moves these clouds: no wind drift of the fields or the noise.
  pClouds->SetWindSpeed(0.0f);
  pClouds->SetEvolutionSpeed(0.0f);
  pClouds->SetCirrusEnabled(false);
  pClouds->SetRenderDistance(160000.0f);
  pClouds->SetAerialPerspective(0.25f);
  pClouds->SetFluidInfluence(1.0f);
  pClouds->SetFluidCoverageScale(1.0f);
  pClouds->SetFluidNdfDetail(0.35f);
  pClouds->SetFluidTypeScale(0.6f);
  pClouds->SetFluidHeightScale(0.45f);
  pClouds->SetFluidFlowPeriod(weather.m_fFlowPeriod);
  return pClouds;
}

void plFluidCloudsApplication::AddWeather(const WeatherSetup& weather)
{
  // The weather map: one Gas volume whose slab holds the middle of the cloud layer.
  m_fWeatherHeight = 0.5f * (weather.m_fLayerMin + weather.m_fLayerMax);

  plGameObjectDesc desc;
  desc.m_sName.Assign("Weather");
  desc.m_LocalPosition.Set(0.0f, 0.0f, m_fWeatherHeight);
  plGameObject* pObject = nullptr;
  m_pWorld->CreateObject(desc, pObject);

  plFluidVolumeComponent* pVolume = nullptr;
  m_pWorld->GetOrCreateComponentManager<plFluidVolumeComponentManager>()->CreateComponent(pObject, pVolume);
  pVolume->SetExtents(plVec2(weather.m_fExtent));
  pVolume->SetHeightRange(weather.m_fLayerMax - weather.m_fLayerMin);
  pVolume->m_fLodDistance = 1.0e7f; // a sky volume is kilometres from the camera; never coarsen or freeze it
  pVolume->m_fSleepAfter = 1.0e9f;
  pVolume->m_bUseWind = false;
  pVolume->m_bUseTouchSources = false;
  pVolume->m_bUseWeather = false;
  pVolume->m_iPriority = 100;

  plFluidSettings& s = pVolume->m_OverrideSettings;
  s.m_Mode = plFluidMode::Gas;
  s.m_Boundary = plFluidBoundary::Free;
  s.m_Advection = plFluidAdvection::MacCormack;
  s.m_uiResolution = weather.m_uiResolution;
  s.m_uiPressureIterations = weather.m_uiPressureIterations;
  s.m_bWarmStartPressure = plEnvironmentVariableUtils::GetValueInt("PL_FLUIDCLOUDS_WARMSTART", 1) != 0;
  s.m_bDrainRemovesGas = false; // the inflow builds cloud; the eye and the beam clear it with sources of their own
  s.m_fDensityDissipation = 1.0f;
  s.m_fVelocityDissipation = weather.m_fVelocityDissipation;
  s.m_fTemperatureDissipation = weather.m_fTemperatureDissipation;
  s.m_fBuoyancy = 0.0f;       // a horizontal plane: in-plane buoyancy would push the clouds sideways
  s.m_fDensityWeight = 0.0f;
  s.m_fVorticity = weather.m_fVorticity;
  plStringBuilder sVorticity = plEnvironmentVariableUtils::GetValueString("PL_FLUIDCLOUDS_VORTICITY");
  if (!sVorticity.IsEmpty())
    s.m_fVorticity = static_cast<float>(atof(sVorticity.GetData()));
  s.m_fAmbientDensity = weather.m_fAmbientDensity;
  s.m_fAmbientRate = weather.m_fAmbientRate;
  s.m_fAmbientScale = weather.m_fAmbientScale;
  s.m_fAmbientBreakup = weather.m_fAmbientBreakup;
  s.m_fEdgeFade = weather.m_fExtent * 0.08f;
  pVolume->SetOverrideSettings(true);
}

plFluidEmitterComponent* plFluidCloudsApplication::AddEmitter(const char* szName, const plVec2& vPos)
{
  plGameObjectDesc desc;
  desc.m_sName.Assign(szName);
  desc.m_LocalPosition.Set(vPos.x, vPos.y, m_fWeatherHeight);
  desc.m_bDynamic = true;
  plGameObject* pObject = nullptr;
  m_pWorld->CreateObject(desc, pObject);

  plFluidEmitterComponent* pEmitter = nullptr;
  m_pWorld->GetOrCreateComponentManager<plFluidEmitterComponentManager>()->CreateComponent(pObject, pEmitter);
  pEmitter->m_fDensity = 0.0f;
  pEmitter->m_fTemperature = 0.0f;
  pEmitter->m_bTrail = false;
  return pEmitter;
}

void plFluidCloudsApplication::SetupHurricane()
{
  WeatherSetup weather;
  weather.m_fLayerMin = 1200.0f;
  weather.m_fLayerMax = 7500.0f;
  weather.m_fExtent = 96000.0f;
  // The environment: scattered fair-weather cumulus, which the circulation draws in and shreds.
  weather.m_fAmbientDensity = 0.5f;
  weather.m_fAmbientRate = 0.0008f;
  weather.m_fAmbientScale = 5000.0f;
  weather.m_fAmbientBreakup = 0.75f;
  weather.m_fVorticity = 0.0f; // confinement scales with step and texel size: at time-lapse steps it blows the solver up
  weather.m_fVelocityDissipation = 0.998f;
  weather.m_fTemperatureDissipation = 0.98f;
  weather.m_fFlowPeriod = 100.0f; // with 2 s steps, a cycle every 50 frames

  // The sea casts no shadow: a 400 km caster only adds acne to itself.
  AddBox("Ocean", plVec3(0.0f, 0.0f, -10.0f), plVec3(400000.0f, 400000.0f, 20.0f), plColor(0.015f, 0.04f, 0.07f), false, false);
  plVolumetricCloudsComponent* pClouds = AddClouds(weather);
  // Beyond the fluid the engine's default weather map takes over; scaled down, only its densest cells survive, as
  // scattered cumulus. The fluid's own scale undoes the reduction, and maps density 1 to a coverage well inside the
  // band the noise sculpts, so the bands keep soft, broken edges instead of solid walls.
  pClouds->SetCoverageScale(0.4f);
  pClouds->SetFluidCoverageScale(0.8f / 0.4f);
  pClouds->SetFluidNdfDetail(0.6f / 0.4f);
  // The cirrus canopy the storm's outflow spreads over its core: the cirrus coverage follows the fluid's density, and
  // the bias keeps it off everywhere the storm is not dense.
  pClouds->SetCirrusEnabled(true);
  pClouds->SetCirrusAltitude(9500.0f);
  pClouds->SetCirrusCoverageBias(-0.25f);
  pClouds->SetCirrusDensity(0.8f);
  AddWeather(weather);

  // The circulation: a wide cyclonic swirl with inflow, and the tight core inside it. Positive swirl turns
  // counter-clockwise seen from above, as in the northern hemisphere. Inside a Rankine core both speeds grow with the
  // radius, so anything the inflow carries follows a logarithmic spiral with a pitch of atan(drain / swirl): about 18
  // degrees here, the pitch of real rainbands.
  plFluidEmitterComponent* pOuter = AddEmitter("Circulation", plVec2::MakeZero());
  pOuter->m_Shape = plFluidEmitterShape::Vortex;
  pOuter->m_fRadius = 16000.0f;
  pOuter->m_fSwirl = 24.0f;
  pOuter->m_fDrain = 8.0f;

  plFluidEmitterComponent* pCore = AddEmitter("Core", plVec2::MakeZero());
  pCore->m_Shape = plFluidEmitterShape::Vortex;
  pCore->m_fRadius = 4000.0f;
  pCore->m_fSwirl = 50.0f;
  pCore->m_fDrain = 6.0f;

  // Rainbands: moist air feeding in from a few places on the storm's edge. The flow draws each plume into a spiral arm
  // that winds into the eyewall, the way dye spirals into a drain.
  for (plUInt32 i = 0; i < 5; ++i)
  {
    const plAngle angle = plAngle::MakeFromDegree(i * 72.0f + 25.0f * Hash01(i, 11));
    const float fRadius = 30000.0f + 6000.0f * Hash01(i, 12);
    plFluidEmitterComponent* pBand = AddEmitter("Rainband", plVec2(plMath::Cos(angle), plMath::Sin(angle)) * fRadius);
    pBand->m_Shape = plFluidEmitterShape::Sphere;
    pBand->m_fRadius = 3200.0f + 1500.0f * Hash01(i, 13);
    pBand->m_fDensity = 0.03f;
    pBand->m_fTemperature = 0.02f;
  }

  // The eyewall: a ring of deep convection that lifts the cloud tops, with a few hot towers where it bursts higher.
  plFluidEmitterComponent* pEyewall = AddEmitter("Eyewall", plVec2::MakeZero());
  pEyewall->m_Shape = plFluidEmitterShape::Ring;
  pEyewall->m_fRadius = 5500.0f;
  pEyewall->m_vExtents.Set(1500.0f, 1500.0f, 1.0f);
  pEyewall->m_fDensity = 0.002f;
  pEyewall->m_fTemperature = 0.05f;

  for (plUInt32 i = 0; i < 3; ++i)
  {
    const plAngle angle = plAngle::MakeFromDegree(40.0f + i * 125.0f);
    plFluidEmitterComponent* pTower = AddEmitter("HotTower", plVec2(plMath::Cos(angle), plMath::Sin(angle)) * 5800.0f);
    pTower->m_Shape = plFluidEmitterShape::Sphere;
    pTower->m_fRadius = 1500.0f;
    pTower->m_fDensity = 0.004f;
    pTower->m_fTemperature = 0.25f;
  }

  // The central dense overcast: broad, weak convection around the core that fills the gaps between the inner bands.
  plFluidEmitterComponent* pOvercast = AddEmitter("CentralOvercast", plVec2::MakeZero());
  pOvercast->m_Shape = plFluidEmitterShape::Sphere;
  pOvercast->m_fRadius = 13000.0f;
  pOvercast->m_fDensity = 0.0012f;
  pOvercast->m_fTemperature = 0.01f;

  // The eye: sinking air that evaporates the cloud.
  plFluidEmitterComponent* pEye = AddEmitter("Eye", plVec2::MakeZero());
  pEye->m_Shape = plFluidEmitterShape::Sphere;
  pEye->m_fRadius = 3000.0f;
  pEye->m_fDensity = -0.06f;
}

void plFluidCloudsApplication::SetupBeam()
{
  WeatherSetup weather;
  weather.m_fLayerMin = 1400.0f;
  weather.m_fLayerMax = 3600.0f;
  weather.m_fExtent = 64000.0f;
  weather.m_fAmbientDensity = 0.85f;
  weather.m_fAmbientRate = 0.01f;
  weather.m_fAmbientScale = 2500.0f;
  weather.m_fAmbientBreakup = 0.3f;
  weather.m_fVorticity = 0.006f;
  weather.m_fVelocityDissipation = 0.99f;
  weather.m_fTemperatureDissipation = 0.85f;

  AddBox("Ground", plVec3(0.0f, 0.0f, -10.0f), plVec3(400000.0f, 400000.0f, 20.0f), plColor(0.10f, 0.13f, 0.06f), false, false);

  // A town around the beam's foot, for scale.
  for (plUInt32 i = 0; i < 90; ++i)
  {
    const float fAngle = Hash01(i, 1) * 6.2831853f;
    const float fDistance = 150.0f + 2600.0f * plMath::Sqrt(Hash01(i, 2));
    const float fWidth = 30.0f + 70.0f * Hash01(i, 3);
    const float fDepth = 30.0f + 70.0f * Hash01(i, 4);
    const float fHeight = 15.0f + 140.0f * Hash01(i, 5) * Hash01(i, 6);
    const float fShade = 0.35f + 0.3f * Hash01(i, 7);
    AddBox("Building", plVec3(plMath::Cos(plAngle::MakeFromRadian(fAngle)) * fDistance, plMath::Sin(plAngle::MakeFromRadian(fAngle)) * fDistance, fHeight * 0.5f),
      plVec3(fWidth, fDepth, fHeight), plColor(fShade, fShade * 0.97f, fShade * 0.92f));
  }

  AddClouds(weather);
  AddWeather(weather);

  // Off until the beam fires (SetFrame). The outflow is a negative drain: a source of clear air at the beam's foot in
  // the deck that pushes the clouds out, with a little swirl so the hole does not open as a perfect circle.
  plFluidEmitterComponent* pBeam = AddEmitter("BeamOutflow", plVec2::MakeZero());
  pBeam->m_Shape = plFluidEmitterShape::Vortex;
  pBeam->m_fRadius = 800.0f;
  m_hBeam = pBeam->GetHandle();

  plFluidEmitterComponent* pClear = AddEmitter("BeamClear", plVec2::MakeZero());
  pClear->m_Shape = plFluidEmitterShape::Sphere;
  pClear->m_fRadius = 900.0f;
  m_hBeamClear = pClear->GetHandle();

  m_hBeamRing.Invalidate();

  // The column of light, grown from the ground when the beam fires (SetFrame).
  // Bright enough that bloom gives it a halo; nearly white, so tonemapping does not push it toward violet.
  m_hBeamColumn = AddBox("BeamColumn", plVec3(0.0f, 0.0f, -100.0f), plVec3(110.0f, 110.0f, 1.0f), plColor(52.0f, 56.0f, 60.0f), true)->GetHandle();
}

plResult plFluidCloudsApplication::SetupClip(Clip clip)
{
  if (plDynamicCast<plFluidTestGameState*>(GetActiveGameState()) == nullptr)
    return PL_FAILURE;

  SetCVar<plCVarBool>("Rendering.VolumetricClouds.Enabled", true);
  SetCVar<plCVarInt>("Rendering.VolumetricClouds.Quality", 3);
  // Screen-space contact shadows hatch a flat ground seen from kilometres away (engine issue, open).
  SetCVar<plCVarBool>("Rendering.ContactShadows.Enable", false);
  ApplyCVarOverrides();
  // The clouds' shaders run on the global clock; a fixed step keeps two runs identical.
  plClock::GetGlobalClock()->SetFixedTimeStep(plTime::MakeFromSeconds(1.0 / 30.0));

  PL_LOCK(m_pWorld->GetWriteMarker());
  m_pWorld->Clear();
  m_pWorld->GetRandomNumberGenerator().Initialize(0x464C5544u);
  m_pWorld->GetClock().Reset(false);
  m_pWorld->GetOrCreateModule<plFluidWorldModule>()->ResetSimulationClock();
  m_hBeam.Invalidate();
  m_hBeamClear.Invalidate();
  m_hBeamRing.Invalidate();
  m_hBeamColumn.Invalidate();

  AddSunAndSky(11.0f, true);

  if (clip == Clip::HurricaneOrbit || clip == Clip::HurricaneEye)
    SetupHurricane();
  else
    SetupBeam();

  SetFrame(clip, -GetClipInfo(clip).m_iWarmupFrames, 0.0f);
  return PL_SUCCESS;
}

void plFluidCloudsApplication::SetFrame(Clip clip, plInt32 iFrame, float fFluidSeconds)
{
  PL_IGNORE_UNUSED(fFluidSeconds);
  PL_LOCK(m_pWorld->GetWriteMarker());

  const float t = plMath::Max(iFrame, 0) / 30.0f; // video seconds
  const ClipInfo& info = GetClipInfo(clip);
  const float fClipLength = info.m_iFrames / 30.0f;

  switch (clip)
  {
    case Clip::HurricaneOrbit:
    {
      // A slow arc above the storm, a third of the way round over the clip.
      const plAngle a = plAngle::MakeFromDegree(-110.0f + 40.0f * (t / fClipLength));
      SetCamera(plVec3(plMath::Cos(a) * 42000.0f, plMath::Sin(a) * 42000.0f, 38000.0f), plVec3(0.0f, 0.0f, 2500.0f), 55.0f);
      SetSun(38.0f, 140.0f);
      break;
    }

    case Clip::HurricaneEye:
    {
      // Standing in the eye below the cloud base, turning to look along the eyewall.
      const plAngle yaw = plAngle::MakeFromDegree(20.0f + 100.0f * (t / fClipLength));
      const plVec3 vPos(-600.0f, -400.0f, 900.0f);
      SetCamera(vPos, vPos + plVec3(plMath::Cos(yaw), plMath::Sin(yaw), 0.42f) * 1000.0f, 75.0f);
      SetSun(62.0f, 200.0f);
      break;
    }

    case Clip::BeamGround:
    case Clip::BeamAerial:
    {
      if (clip == Clip::BeamGround)
        SetCamera(plVec3(-300.0f, -4600.0f, 2.0f), plVec3(0.0f, 0.0f, 2300.0f), 72.0f);
      else
        SetCamera(plVec3(5000.0f, -10000.0f, 7500.0f), plVec3(0.0f, 0.0f, 2200.0f), 55.0f);
      // The sun behind the beam, so the shaft it lets through lands between the beam and the camera. From above, a
      // lower sun models the deck's relief and the walls of the hole.
      if (clip == Clip::BeamGround)
        SetSun(52.0f, 95.0f);
      else
        SetSun(28.0f, 60.0f);

      // The beam fires 1.5 s in, ramps up over half a second and stays on.
      const float fOn = SmoothStep01((t - 1.5f) / 0.5f) * (iFrame >= 0 ? 1.0f : 0.0f);
      plFluidEmitterComponent* pBeam = nullptr;
      if (m_pWorld->TryGetComponent(m_hBeam, pBeam))
      {
        pBeam->m_fDrain = -110.0f * fOn;
        pBeam->m_fSwirl = 8.0f * fOn;
        pBeam->m_fTemperature = 0.8f * fOn;
      }
      plFluidEmitterComponent* pClear = nullptr;
      if (m_pWorld->TryGetComponent(m_hBeamClear, pClear))
        pClear->m_fDensity = -3.0f * fOn;

      // The column shoots up from the ground in a fifth of a second, then pulses faintly.
      plGameObject* pColumn = nullptr;
      if (m_pWorld->TryGetObject(m_hBeamColumn, pColumn))
      {
        const float fHeight = 14000.0f * SmoothStep01((t - 1.3f) / 0.2f) * (iFrame >= 0 ? 1.0f : 0.0f);
        const float fPulse = 1.0f + 0.15f * plMath::Sin(plAngle::MakeFromRadian(t * 9.0f));
        pColumn->SetLocalPosition(plVec3(0.0f, 0.0f, fHeight * 0.5f));
        pColumn->SetLocalScaling(plVec3(110.0f * fPulse, 110.0f * fPulse, plMath::Max(fHeight, 0.01f)));
      }
      break;
    }

    default:
      break;
  }
}

plResult plFluidCloudsApplication::SaveFrame(Clip clip, plInt32 iFrame)
{
  plStringBuilder sPath;
  sPath.SetFormat(":imgout/{}/frame_{}.png", GetClipInfo(clip).m_szName, plArgI(iFrame, 5, true));
  return GetLastScreenshot().SaveTo(sPath);
}

plResult plFluidCloudsApplication::WriteTitle(Clip clip)
{
  plStringBuilder sPath;
  sPath.SetFormat(":imgout/{}/title.txt", GetClipInfo(clip).m_szName);

  plFileWriter file;
  PL_SUCCEED_OR_RETURN(file.Open(sPath));
  plStringBuilder sText(GetClipInfo(clip).m_szTitle, "\n");
  return file.WriteBytes(sText.GetData(), sText.GetElementCount());
}

//////////////////////////////////////////////////////////////////////////
// Test

const char* plGameEngineTestFluidClouds::GetTestName() const
{
  return "Fluid Clouds";
}

plGameEngineTestApplication* plGameEngineTestFluidClouds::CreateApplication()
{
  m_pOwnApplication = PL_DEFAULT_NEW(plFluidCloudsApplication);
  return m_pOwnApplication;
}

void plGameEngineTestFluidClouds::SetupSubTests()
{
  AddSubTest("HurricaneOrbit", (plInt32)plFluidCloudsApplication::Clip::HurricaneOrbit);
  AddSubTest("HurricaneEye", (plInt32)plFluidCloudsApplication::Clip::HurricaneEye);
  AddSubTest("BeamGround", (plInt32)plFluidCloudsApplication::Clip::BeamGround);
  AddSubTest("BeamAerial", (plInt32)plFluidCloudsApplication::Clip::BeamAerial);
}

plResult plGameEngineTestFluidClouds::InitializeSubTest(plInt32 iIdentifier)
{
  PL_SUCCEED_OR_RETURN(SUPER::InitializeSubTest(iIdentifier));

  const auto clip = static_cast<plFluidCloudsApplication::Clip>(iIdentifier);
  m_iClipFrame = 0;
  m_fFluidSeconds = 0.0;

  // PL_FLUIDCLOUDS_FRAMES shortens a clip and PL_FLUIDCLOUDS_SAVE_EVERY writes every Nth frame, for quick looks.
  m_iFramesToWrite = plFluidCloudsApplication::GetClipInfo(clip).m_iFrames;
  const plInt32 iFrames = plEnvironmentVariableUtils::GetValueInt("PL_FLUIDCLOUDS_FRAMES", 0);
  if (iFrames > 0)
    m_iFramesToWrite = plMath::Min(m_iFramesToWrite, iFrames);
  m_iSaveEvery = plMath::Max(plEnvironmentVariableUtils::GetValueInt("PL_FLUIDCLOUDS_SAVE_EVERY", 1), 1);

  PL_SUCCEED_OR_RETURN(m_pOwnApplication->SetupClip(clip));
  return m_pOwnApplication->WriteTitle(clip);
}

plTestAppRun plGameEngineTestFluidClouds::RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount)
{
  PL_IGNORE_UNUSED(uiInvocationCount);

  const auto clip = static_cast<plFluidCloudsApplication::Clip>(iIdentifier);
  const plFluidCloudsApplication::ClipInfo& info = plFluidCloudsApplication::GetClipInfo(clip);
  const plInt32 iFrame = m_iClipFrame - info.m_iWarmupFrames;

  // Fluid time-lapse: every frame simulates a fixed number of fluid seconds, whatever the frame took.
  float fWarmupStep = info.m_fWarmupStep;
  plStringBuilder sWarmupStep = plEnvironmentVariableUtils::GetValueString("PL_FLUIDCLOUDS_WARMUP_STEP");
  if (!sWarmupStep.IsEmpty())
    fWarmupStep = static_cast<float>(atof(sWarmupStep.GetData()));
  const float fStep = iFrame < 0 ? fWarmupStep : info.m_fStep;
  SetCVar<plCVarFloat>("Fluid.Debug.StepTime", fStep);
  SetCVar<plCVarFloat>("Fluid.MaxStepTime", fStep);

  m_pOwnApplication->SetFrame(clip, iFrame, static_cast<float>(m_fFluidSeconds));

  if (m_pOwnApplication->Run() == plApplication::Execution::Quit)
    return plTestAppRun::Quit;

  m_fFluidSeconds += fStep;
  ++m_iClipFrame;

  if (iFrame < 0)
    return plTestAppRun::Continue;

  if (iFrame % m_iSaveEvery == 0)
    PL_TEST_BOOL(m_pOwnApplication->SaveFrame(clip, iFrame / m_iSaveEvery).Succeeded());

  return iFrame + 1 >= m_iFramesToWrite ? plTestAppRun::Quit : plTestAppRun::Continue;
}
