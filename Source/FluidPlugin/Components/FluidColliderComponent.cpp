#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidColliderComponent.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidColliderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Shape", plFluidColliderShape, m_Shape),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.01f, 100.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.5f, 0.5f, 0.5f)), new plClampValueAttribute(plVec3(0.01f), plVec3(100.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Static", m_bStatic)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Slip", m_fSlip)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/Fluid"),
    new plBoxVisualizerAttribute("Extents", 2.0f, plColor::Orange),
    new plSphereVisualizerAttribute("Radius", plColor::Orange),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFluidColliderComponent::plFluidColliderComponent() = default;
plFluidColliderComponent::~plFluidColliderComponent() = default;

void plFluidColliderComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_Shape;
  s << m_fRadius;
  s << m_vExtents;
  s << m_bStatic;
  s << m_fSlip;
}

void plFluidColliderComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_Shape;
  s >> m_fRadius;
  s >> m_vExtents;
  s >> m_bStatic;
  s >> m_fSlip;
}

bool plFluidColliderComponent::FillCollider(plFluidWorldCollider& out_collider) const
{
  const plTransform transform = GetOwner()->GetGlobalTransform();
  const float fRadius = plMath::Max(m_fRadius, 0.01f);

  out_collider.m_vPosition = transform.m_vPosition;
  out_collider.m_qRotation = transform.m_qRotation;
  // plFluidWorldCollider carries no radius, so the round shapes pack theirs into the extents: a sphere as (r, r, r), a
  // capsule as (half length, r, r).
  switch (m_Shape)
  {
    case plFluidColliderShape::Sphere:
      out_collider.m_vExtents = plVec3(fRadius);
      break;

    case plFluidColliderShape::Capsule:
      // The obstacle kernel reads a capsule as (half length along local X, radius).
      out_collider.m_vExtents = plVec3(plMath::Max(m_vExtents.x, 0.0f), fRadius, fRadius);
      break;

    default:
      out_collider.m_vExtents = m_vExtents;
      break;
  }
  // The volume fills the in-plane rotation once it knows its own plane normal.
  out_collider.m_fRotation = 0.0f;
  out_collider.m_fSlip = plMath::Clamp(m_fSlip, 0.0f, 1.0f);
  out_collider.m_uiShape = static_cast<plUInt8>(m_Shape.GetValue());

  return true;
}

plTransform plFluidColliderComponent::GetGlobalTransform() const
{
  return GetOwner()->GetGlobalTransform();
}

bool plFluidColliderComponent::HasMovedSince(const plTransform& last) const
{
  return !GetOwner()->GetGlobalTransform().IsEqual(last, 0.0005f);
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidColliderComponent);
