#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidSmokeRenderComponent.h>
#include <FluidPlugin/Components/FluidVolumeComponent.h>
#include <FluidPlugin/FluidMessages.h>
#include <FluidPlugin/World/FluidWorldModule.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidVolumeComponent, 5, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("Preset", GetPresetFile, SetPresetFile)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Fluid_Preset")),
    PL_ACCESSOR_PROPERTY("OverrideMode", GetOverrideMode, SetOverrideMode),
    PL_ENUM_ACCESSOR_PROPERTY("Mode", plFluidMode, GetFluidMode, SetFluidMode),
    PL_ACCESSOR_PROPERTY("Extents", GetExtents, SetExtents)->AddAttributes(new plDefaultValueAttribute(plVec2(16.0f, 16.0f)), new plClampValueAttribute(plVec2(0.5f), plVec2(2048.0f)), new plSuffixAttribute(" m")),
    PL_ACCESSOR_PROPERTY("OverrideResolution", GetOverrideResolution, SetOverrideResolution),
    PL_ACCESSOR_PROPERTY("Resolution", GetResolution, SetResolution)->AddAttributes(new plDefaultValueAttribute(256), new plClampValueAttribute(64, 1024)),
    PL_ACCESSOR_PROPERTY("FollowTarget", DummyGetter, SetFollowTargetReference)->AddAttributes(new plGameObjectReferenceAttribute()),
    PL_MEMBER_PROPERTY("FollowMainCamera", m_bFollowMainCamera),
    PL_ACCESSOR_PROPERTY("HeightRange", GetHeightRange, SetHeightRange)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.01f, 500.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("UseTouchSources", m_bUseTouchSources)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("TouchObstacles", m_bTouchObstacles)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("UseWind", m_bUseWind)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("UseWeather", m_bUseWeather)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Precipitation", m_fPrecipitation)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("InitialDepth", m_fInitialDepth)->AddAttributes(new plClampValueAttribute(0.0f, 10.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("SleepAfter", m_fSleepAfter)->AddAttributes(new plDefaultValueAttribute(3.0f), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("LodDistance", m_fLodDistance)->AddAttributes(new plDefaultValueAttribute(60.0f), new plClampValueAttribute(1.0f, 10000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Priority", m_iPriority),
    PL_ENUM_MEMBER_PROPERTY("DebugView", plFluidDebugView, m_DebugView),
    PL_MEMBER_PROPERTY("LandFromScene", m_bLandFromScene)->AddAttributes(new plGroupAttribute("Shore")),
    PL_MEMBER_PROPERTY("LandCollisionLayer", m_uiLandCollisionLayer)->AddAttributes(new plDynamicEnumAttribute("PhysicsCollisionLayer"), new plGroupAttribute("Shore")),
    PL_MEMBER_PROPERTY("ShoreFoam", m_fShoreFoam)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Shore")),
    PL_MEMBER_PROPERTY("ShoreDamping", m_fShoreDamping)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Shore")),
    PL_ACCESSOR_PROPERTY("OverrideSettings", GetOverrideSettings, SetOverrideSettings)->AddAttributes(new plGroupAttribute("Settings")),
    PL_MEMBER_PROPERTY("Settings", m_OverrideSettings)->AddAttributes(new plGroupAttribute("Settings")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/Fluid"),
    new plDirectionVisualizerAttribute(plBasisAxis::PositiveZ, 1.0f, plColor::DeepSkyBlue),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgUpdateLocalBounds, OnUpdateLocalBounds),
    PL_MESSAGE_HANDLER(plMsgFluidImpulse, OnMsgFluidImpulse),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFluidVolumeComponent::plFluidVolumeComponent() = default;
plFluidVolumeComponent::~plFluidVolumeComponent() = default;

void plFluidVolumeComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  inout_stream.WriteGameObjectHandle(m_hFollowTarget);

  s << m_hPreset;
  s << m_bOverrideMode;
  s << m_Mode;
  s << m_vExtents;
  s << m_bOverrideResolution;
  s << m_uiResolution;
  s << m_bFollowMainCamera;
  s << m_fHeightRange;
  s << m_bUseTouchSources;
  s << m_bUseWind;
  s << m_bUseWeather;
  s << m_fPrecipitation;
  s << m_fInitialDepth;
  s << m_fSleepAfter;
  s << m_fLodDistance;
  s << m_iPriority;
  s << m_DebugView;
  s << m_bLandFromScene;
  s << m_uiLandCollisionLayer;
  s << m_fShoreFoam;
  s << m_fShoreDamping;

  s << m_bOverrideSettings;
  s << m_bTouchObstacles;
  s << plFluidSettings::SerializationVersion;
  m_OverrideSettings.Save(s);
}

void plFluidVolumeComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  auto& s = inout_stream.GetStream();

  m_hFollowTarget = inout_stream.ReadGameObjectHandle();

  s >> m_hPreset;
  s >> m_bOverrideMode;
  s >> m_Mode;
  // Before version 5 the mode could be Liquid or Flow, both Water now.
  m_Mode = plFluidMode::FromStoredValue(m_Mode.GetValue());
  s >> m_vExtents;
  s >> m_bOverrideResolution;
  s >> m_uiResolution;
  s >> m_bFollowMainCamera;
  s >> m_fHeightRange;
  s >> m_bUseTouchSources;
  s >> m_bUseWind;
  s >> m_bUseWeather;

  // Version 2 added the volume's own precipitation and its starting snow depth.
  if (uiVersion >= 2)
  {
    s >> m_fPrecipitation;
    s >> m_fInitialDepth;
  }
  s >> m_fSleepAfter;
  s >> m_fLodDistance;
  s >> m_iPriority;
  s >> m_DebugView;

  // Version 3 added the land bake and the shore parameters.
  if (uiVersion >= 3)
  {
    s >> m_bLandFromScene;
    s >> m_uiLandCollisionLayer;
    s >> m_fShoreFoam;
    s >> m_fShoreDamping;
  }

  // Version 4 added the volume's own settings and physics bodies as obstacles. The settings carry their own layout
  // version, so a new settings field needs no component version bump.
  if (uiVersion >= 4)
  {
    s >> m_bOverrideSettings;
    s >> m_bTouchObstacles;

    plUInt8 uiSettingsVersion = 0;
    s >> uiSettingsVersion;
    if (m_OverrideSettings.Load(s, uiSettingsVersion).Failed())
    {
      m_OverrideSettings = plFluidSettings();
      m_bOverrideSettings = false;
    }
  }

  m_bSettingsDirty = true;
}

void plFluidVolumeComponent::OnActivated()
{
  SUPER::OnActivated();

  m_bSettingsDirty = true;
  m_hVolume = plFluidGpuManager::AllocateVolume(GetWorld()->GetIndex());

  if (plFluidWorldModule* pModule = GetWorld()->GetOrCreateModule<plFluidWorldModule>())
  {
    pModule->RegisterVolume(this);
  }

  GetOwner()->UpdateLocalBounds();
}

void plFluidVolumeComponent::OnDeactivated()
{
  if (plFluidWorldModule* pModule = GetWorld()->GetModule<plFluidWorldModule>())
  {
    pModule->DeregisterVolume(this);
  }

  plFluidGpuManager::Free(GetWorld()->GetIndex(), m_hVolume);

  GetOwner()->UpdateLocalBounds();

  SUPER::OnDeactivated();
}

void plFluidVolumeComponent::SetPreset(const plFluidPresetResourceHandle& hPreset)
{
  m_hPreset = hPreset;
  m_bSettingsDirty = true;
}

const plFluidPresetResourceHandle& plFluidVolumeComponent::GetPreset() const
{
  return m_hPreset;
}

void plFluidVolumeComponent::SetOverrideMode(bool bOverride)
{
  m_bOverrideMode = bOverride;
  m_bSettingsDirty = true;
}

void plFluidVolumeComponent::SetFluidMode(plEnum<plFluidMode> mode)
{
  m_Mode = mode;
  m_bSettingsDirty = true;
}

void plFluidVolumeComponent::SetOverrideResolution(bool bOverride)
{
  m_bOverrideResolution = bOverride;
  m_bSettingsDirty = true;
}

void plFluidVolumeComponent::SetResolution(plUInt32 uiResolution)
{
  m_uiResolution = uiResolution;
  m_bSettingsDirty = true;
}

void plFluidVolumeComponent::SetOverrideSettings(bool bOverride)
{
  m_bOverrideSettings = bOverride;
  m_bSettingsDirty = true;
}

void plFluidVolumeComponent::SetExtents(plVec2 vExtents)
{
  m_vExtents.x = plMath::Max(vExtents.x, 0.01f);
  m_vExtents.y = plMath::Max(vExtents.y, 0.01f);

  if (IsActiveAndInitialized())
  {
    GetOwner()->UpdateLocalBounds();
  }
}

void plFluidVolumeComponent::SetHeightRange(float fHeightRange)
{
  m_fHeightRange = plMath::Max(fHeightRange, 0.01f);

  if (IsActiveAndInitialized())
  {
    GetOwner()->UpdateLocalBounds();
  }
}

void plFluidVolumeComponent::SetFollowTargetReference(const char* szReference)
{
  auto resolver = GetWorld()->GetGameObjectReferenceResolver();

  if (!resolver.IsValid())
    return;

  m_hFollowTarget = resolver(szReference, GetHandle(), "FollowTarget");
}

const plFluidSettings& plFluidVolumeComponent::GetEffectiveSettings() const
{
  // The editor writes the embedded Settings through reflection without a setter, so while the override is on the
  // settings are rebuilt on every call; that is a copy, no resource access.
  if (m_bSettingsDirty || m_bOverrideSettings)
  {
    m_Settings = plFluidSettings();

    if (m_bOverrideSettings)
    {
      m_Settings = m_OverrideSettings;
    }
    else if (m_hPreset.IsValid())
    {
      plResourceLock<plFluidPresetResource> pPreset(m_hPreset, plResourceAcquireMode::BlockTillLoaded_NeverFail);
      if (pPreset.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        m_Settings = pPreset->GetSettings();
      }
    }

    if (m_bOverrideMode)
    {
      m_Settings.m_Mode = m_Mode;
    }

    if (m_bOverrideResolution)
    {
      m_Settings.m_uiResolution = m_uiResolution;
    }

    m_bSettingsDirty = false;
  }

  return m_Settings;
}

void plFluidVolumeComponent::BuildInput(plFluidVolumeInput& out_input) const
{
  out_input.m_Transform = GetOwner()->GetGlobalTransform();
  out_input.m_vExtents = m_vExtents;
  // Height Range is the full slab; the manager wants the reach to each side of the plane.
  out_input.m_fHalfThickness = m_fHeightRange * 0.5f;
  out_input.m_Settings = GetEffectiveSettings();
  out_input.m_iPriority = m_iPriority;
  out_input.m_fSleepAfter = m_fSleepAfter;
  out_input.m_fInitialDepth = m_fInitialDepth;
  out_input.m_fPrecipitation = m_fPrecipitation;
  out_input.m_fShoreFoam = plMath::Max(m_fShoreFoam, 0.0f);
  out_input.m_fShoreDamping = plMath::Max(m_fShoreDamping, 0.0f);

  // A smoke render component on the same object asks for the 3D smoke texture.
  const plFluidSmokeRenderComponent* pSmoke = nullptr;
  if (GetOwner()->TryGetComponentOfBaseType(pSmoke) && pSmoke->IsActiveAndInitialized())
  {
    out_input.m_bSmokeVolume = true;
    out_input.m_uiSmokeDepth = pSmoke->m_uiDepth;
    out_input.m_fSmokeFalloff = pSmoke->m_fFalloff;
  }
}

void plFluidVolumeComponent::OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg)
{
  const plVec3 vHalfExtents(m_vExtents.x * 0.5f, m_vExtents.y * 0.5f, m_fHeightRange * 0.5f);
  const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), vHalfExtents);

  msg.AddBounds(plBoundingBoxSphere::MakeFromBox(box),
    GetOwner()->IsDynamic() ? plDefaultSpatialDataCategories::RenderDynamic : plDefaultSpatialDataCategories::RenderStatic);
}

void plFluidVolumeComponent::OnMsgFluidImpulse(plMsgFluidImpulse& msg)
{
  // The impulse is queued on this volume's world, so every volume of that world whose box contains the position
  // receives it, not only this component.
  if (plFluidWorldModule* pModule = GetWorld()->GetModule<plFluidWorldModule>())
  {
    pModule->AddImpulse(msg.m_vPosition, msg.m_fRadius, msg.m_vVelocity, msg.m_fAmount, msg.m_fHeightImpulse);
  }
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidVolumeComponent);
