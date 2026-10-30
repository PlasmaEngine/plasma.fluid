#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/Messages/UpdateLocalBoundsMessage.h>
#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidSmokeRenderComponent.h>
#include <FluidPlugin/Components/FluidVolumeComponent.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Volumes/VDBVolumeComponent.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidSmokeRenderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("DensityScale", m_fDensityScale)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Albedo", m_Albedo)->AddAttributes(new plDefaultValueAttribute(plColor::White)),
    PL_MEMBER_PROPERTY("EmissionColor", m_EmissionColor)->AddAttributes(new plDefaultValueAttribute(plColor::Black)),
    PL_MEMBER_PROPERTY("EmissionScale", m_fEmissionScale)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(0.0f)),
    PL_MEMBER_PROPERTY("Anisotropy", m_fAnisotropy)->AddAttributes(new plClampValueAttribute(-0.99f, 0.99f), new plDefaultValueAttribute(0.3f)),
    PL_MEMBER_PROPERTY("Depth", m_uiDepth)->AddAttributes(new plClampValueAttribute(4, 64), new plDefaultValueAttribute(16)),
    PL_MEMBER_PROPERTY("Falloff", m_fFalloff)->AddAttributes(new plClampValueAttribute(0.0f, 16.0f), new plDefaultValueAttribute(1.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/Fluid"),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgUpdateLocalBounds, OnUpdateLocalBounds),
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFluidSmokeRenderComponent::plFluidSmokeRenderComponent() = default;
plFluidSmokeRenderComponent::~plFluidSmokeRenderComponent() = default;

void plFluidSmokeRenderComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_fDensityScale;
  s << m_Albedo;
  s << m_EmissionColor;
  s << m_fEmissionScale;
  s << m_fAnisotropy;
  s << m_uiDepth;
  s << m_fFalloff;
}

void plFluidSmokeRenderComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_fDensityScale;
  s >> m_Albedo;
  s >> m_EmissionColor;
  s >> m_fEmissionScale;
  s >> m_fAnisotropy;
  s >> m_uiDepth;
  s >> m_fFalloff;
}

void plFluidSmokeRenderComponent::OnActivated()
{
  SUPER::OnActivated();

  GetOwner()->UpdateLocalBounds();
}

void plFluidSmokeRenderComponent::OnDeactivated()
{
  GetOwner()->UpdateLocalBounds();

  SUPER::OnDeactivated();
}

void plFluidSmokeRenderComponent::OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg)
{
  // The smoke fills the sibling volume's box.
  const plFluidVolumeComponent* pVolume = nullptr;
  if (!GetOwner()->TryGetComponentOfBaseType(pVolume) || !pVolume->IsActiveAndInitialized())
    return;

  const plVec2 vExtents = pVolume->GetExtents();
  const plVec3 vHalfExtents(vExtents.x * 0.5f, vExtents.y * 0.5f, pVolume->GetHeightRange() * 0.5f);
  const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), vHalfExtents);

  msg.AddBounds(plBoundingBoxSphere::MakeFromBox(box),
    GetOwner()->IsDynamic() ? plDefaultSpatialDataCategories::RenderDynamic : plDefaultSpatialDataCategories::RenderStatic);
}

void plFluidSmokeRenderComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (msg.m_OverrideCategory != plInvalidRenderDataCategory)
    return;

  const plFluidVolumeComponent* pVolume = nullptr;
  if (!GetOwner()->TryGetComponentOfBaseType(pVolume) || !pVolume->IsActiveAndInitialized())
    return;

  // Invalid unless the volume simulates Gas, has its smoke texture and Fluid.SmokeVolumes is on.
  const plVDBVolumeResourceHandle hSmoke = plFluidGpuManager::GetSmokeVolume(GetWorld()->GetIndex(), pVolume->GetVolumeHandle());
  if (!hSmoke.IsValid())
    return;

  auto pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plVDBVolumeRenderData>(GetOwner());

  // The solver ignores the owner's scale and works in the volume's extents, so the box does too.
  const plTransform globalTransform = GetOwner()->GetGlobalTransform();
  pRenderData->m_hVolume = hSmoke;
  pRenderData->m_vGlobalPosition = globalTransform.m_vPosition;
  pRenderData->m_GlobalRotation = globalTransform.m_qRotation;
  pRenderData->m_vGlobalScale = plVec3(1.0f);

  // Full size of the box: x and y span the grid, z the Height Range the grid is extruded across.
  const plVec2 vExtents = pVolume->GetExtents();
  pRenderData->m_vExtents = plVec3(vExtents.x, vExtents.y, pVolume->GetHeightRange());
  pRenderData->m_vLocalOffset = plVec3::MakeZero();

  pRenderData->m_fDensityScale = m_fDensityScale;
  pRenderData->m_Albedo = m_Albedo;
  pRenderData->m_EmissionColor = m_EmissionColor;
  pRenderData->m_fEmissionScale = m_fEmissionScale;
  pRenderData->m_fAnisotropy = m_fAnisotropy;
  pRenderData->m_fMaxDensity = 4.0f;
  pRenderData->m_Channels = plVDBVolumeChannel::Density | plVDBVolumeChannel::Temperature;
  pRenderData->m_uiFrame = 0;

  // The texture changes every frame, so the render data is never cached.
  msg.AddRenderData(pRenderData, plDefaultRenderDataCategories::Light, plRenderData::Caching::Never);
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidSmokeRenderComponent);
