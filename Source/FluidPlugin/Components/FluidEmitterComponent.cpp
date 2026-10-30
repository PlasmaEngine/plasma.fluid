#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidEmitterComponent.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidEmitterComponent, 2, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Shape", plFluidEmitterShape, m_Shape),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.01f, 100.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Extents", m_vExtents)->AddAttributes(new plDefaultValueAttribute(plVec3(0.5f, 0.5f, 0.5f)), new plClampValueAttribute(plVec3(0.01f), plVec3(100.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Density", m_fDensity)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Temperature", m_fTemperature)->AddAttributes(new plDefaultValueAttribute(0.5f)),
    PL_MEMBER_PROPERTY("Velocity", m_vVelocity),
    PL_MEMBER_PROPERTY("HeightImpulse", m_fHeightImpulse),
    PL_MEMBER_PROPERTY("Press", m_fPress)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Falloff", m_fFalloff)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Burst", m_bBurst),
    PL_MEMBER_PROPERTY("Trail", m_bTrail)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Swirl", m_fSwirl)->AddAttributes(new plDefaultValueAttribute(0.0f), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("Drain", m_fDrain)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" m/s")),
    PL_MEMBER_PROPERTY("OscillationPeriod", m_fOscillationPeriod)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" sec")),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/Fluid"),
    new plSphereVisualizerAttribute("Radius", plColor::DeepSkyBlue),
    new plBoxVisualizerAttribute("Extents", 2.0f, plColor::DeepSkyBlue),
    new plDirectionVisualizerAttribute(plBasisAxis::PositiveX, 0.5f, plColor::DeepSkyBlue),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFluidEmitterComponent::plFluidEmitterComponent() = default;
plFluidEmitterComponent::~plFluidEmitterComponent() = default;

void plFluidEmitterComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_Shape;
  s << m_fRadius;
  s << m_vExtents;
  s << m_fDensity;
  s << m_fTemperature;
  s << m_vVelocity;
  s << m_fHeightImpulse;
  s << m_fPress;
  s << m_fFalloff;
  s << m_bBurst;
  s << m_bTrail;
  s << m_fSwirl;
  s << m_fDrain;
  s << m_fOscillationPeriod;
}

void plFluidEmitterComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  auto& s = inout_stream.GetStream();

  s >> m_Shape;
  s >> m_fRadius;
  s >> m_vExtents;
  s >> m_fDensity;
  s >> m_fTemperature;
  s >> m_vVelocity;
  s >> m_fHeightImpulse;
  s >> m_fPress;
  s >> m_fFalloff;
  s >> m_bBurst;
  s >> m_bTrail;

  // Version 2 added the vortex parameters and the height impulse oscillation.
  if (uiVersion >= 2)
  {
    s >> m_fSwirl;
    s >> m_fDrain;
    s >> m_fOscillationPeriod;
  }
}

void plFluidEmitterComponent::OnActivated()
{
  SUPER::OnActivated();

  m_bHasLastPosition = false;
  m_bBurstConsumed = false;
}

bool plFluidEmitterComponent::FillSource(plFluidWorldSource& out_source, plTime fluidTime) const
{
  if (m_bBurst && m_bBurstConsumed)
    return false;

  const plTransform transform = GetOwner()->GetGlobalTransform();

  out_source.m_vPosition = transform.m_vPosition;
  // Without a trail the segment collapses to a point, so the solver injects at one spot.
  out_source.m_vPrevPosition = (m_bTrail && m_bHasLastPosition) ? m_vLastPosition : transform.m_vPosition;
  out_source.m_vVelocity = transform.m_qRotation * m_vVelocity;
  out_source.m_vExtents = m_vExtents;
  out_source.m_fRadius = plMath::Max(m_fRadius, 0.01f);
  out_source.m_fDensity = m_fDensity;
  out_source.m_fTemperature = m_fTemperature;
  out_source.m_fHeightImpulse = m_fHeightImpulse;
  out_source.m_fPress = m_fPress;
  out_source.m_fFalloff = plMath::Clamp(m_fFalloff, 0.0f, 1.0f);
  out_source.m_fSwirl = m_fSwirl;
  out_source.m_fDrain = plMath::Max(m_fDrain, 0.0f);

  // The phase follows the fluid's own clock, so every oscillating emitter of a world stays in step, and a swell keeps
  // rolling in the editor while the world clock is paused.
  if (m_fOscillationPeriod > 0.0f)
  {
    const double fPeriod = static_cast<double>(m_fOscillationPeriod);
    const double fPhase = plMath::Mod(fluidTime.GetSeconds(), fPeriod) / fPeriod;
    out_source.m_fHeightImpulse *= plMath::Sin(plAngle::MakeFromRadian(static_cast<float>(fPhase * 2.0 * plMath::Pi<double>())));
  }
  out_source.m_uiShape = static_cast<plUInt8>(m_Shape.GetValue());

  // Box: unit direction of the local X. Capsule: half the segment along the local X, Extents.x long.
  out_source.m_vAxis = plVec3::MakeZero();
  if (m_Shape == plFluidEmitterShape::Box)
  {
    out_source.m_vAxis = transform.m_qRotation * plVec3::MakeAxisX();
  }
  else if (m_Shape == plFluidEmitterShape::Capsule)
  {
    out_source.m_vAxis = transform.m_qRotation * plVec3::MakeAxisX() * plMath::Max(m_vExtents.x, 0.0f);
  }

  return true;
}

void plFluidEmitterComponent::AdvanceTrail()
{
  m_vLastPosition = GetOwner()->GetGlobalPosition();
  m_bHasLastPosition = true;

  if (m_bBurst)
  {
    m_bBurstConsumed = true;
  }
}

void plFluidEmitterComponent::ResetBurst()
{
  m_bBurstConsumed = false;
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidEmitterComponent);
