#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/Interfaces/WindWorldModule.h>
#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidDriftSprayComponent.h>
#include <FluidPlugin/Components/FluidVolumeComponent.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Reflection/ReflectionUtils.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidDriftSprayComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Threshold", m_fThreshold)->AddAttributes(new plClampValueAttribute(0.00001f, plVariant()), new plDefaultValueAttribute(0.002f)),
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("ParameterName", GetParameterName, SetParameterName)->AddAttributes(new plDefaultValueAttribute("Drift")),
    PL_MEMBER_PROPERTY("Height", m_fHeight)->AddAttributes(new plDefaultValueAttribute(0.1f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Effects/Fluid"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFluidDriftSprayComponent::plFluidDriftSprayComponent()
{
  m_sParameterName.Assign("Drift");
}

plFluidDriftSprayComponent::~plFluidDriftSprayComponent() = default;

void plFluidDriftSprayComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_fThreshold;
  s << m_fStrength;
  s << m_sParameterName;
  s << m_fHeight;
}

void plFluidDriftSprayComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_fThreshold;
  s >> m_fStrength;
  s >> m_sParameterName;
  s >> m_fHeight;
}

void plFluidDriftSprayComponent::OnDeactivated()
{
  m_fLastValue = -1.0f;

  SUPER::OnDeactivated();
}

void plFluidDriftSprayComponent::Update()
{
  constexpr plUInt32 uiGrid = plFluidGpuManager::SnowActivityGridSize;
  constexpr plUInt32 uiCells = uiGrid * uiGrid;

  plGameObject* pParent = GetOwner()->GetParent();
  const plFluidVolumeComponent* pVolume = nullptr;

  if (pParent == nullptr || !pParent->TryGetComponentOfBaseType(pVolume) || !pVolume->IsActiveAndInitialized() ||
      pVolume->GetEffectiveSettings().m_Mode != plFluidMode::Snow)
  {
    SetEffectParameter(0.0f);
    return;
  }

  float activity[uiCells];
  plUInt32 uiFrame = 0;
  if (!plFluidGpuManager::GetSnowActivity(GetWorld()->GetIndex(), pVolume->GetVolumeHandle(), plMakeArrayPtr(activity, uiCells), uiFrame))
  {
    SetEffectParameter(0.0f);
    return;
  }

  float fTotal = 0.0f;
  for (plUInt32 i = 0; i < uiCells; ++i)
  {
    activity[i] = plMath::Max(activity[i], 0.0f);
    fTotal += activity[i];
  }

  const float fDrift = plMath::Saturate(fTotal / (plMath::Max(m_fThreshold, 0.00001f) * uiCells)) * m_fStrength;

  if (fTotal > 0.0f)
  {
    // A cell with twice the moving snow is picked twice as often, so over a few frames the spray spreads out in
    // proportion to the drift.
    float fPick = GetWorld()->GetRandomNumberGenerator().FloatZeroToOneExclusive() * fTotal;
    plUInt32 uiCell = uiCells - 1;
    for (plUInt32 i = 0; i < uiCells; ++i)
    {
      fPick -= activity[i];
      if (fPick < 0.0f)
      {
        uiCell = i;
        break;
      }
    }

    // Cells are stored row by row, u along the volume's local x. The solver ignores the owner's scale, so this does too.
    const float fU = ((uiCell % uiGrid) + 0.5f) / uiGrid;
    const float fV = ((uiCell / uiGrid) + 0.5f) / uiGrid;
    const plVec2 vExtents = pVolume->GetExtents();

    plTransform volumeTransform = pParent->GetGlobalTransform();
    volumeTransform.m_vScale.Set(1.0f);

    const plVec3 vPosition = volumeTransform.TransformPosition(plVec3((fU - 0.5f) * vExtents.x, (fV - 0.5f) * vExtents.y, m_fHeight));
    GetOwner()->SetGlobalPosition(vPosition);

    if (const plWindWorldModuleInterface* pWind = GetWorld()->GetModuleReadOnly<plWindWorldModuleInterface>())
    {
      const plVec3 vWind = pWind->GetWindAt(vPosition);
      if (vWind.GetLengthSquared() > 0.0001f)
      {
        GetOwner()->SetGlobalRotation(plQuat::MakeShortestRotation(plVec3::MakeAxisX(), vWind.GetNormalized()));
      }
    }
  }

  SetEffectParameter(fDrift);
}

void plFluidDriftSprayComponent::SetEffectParameter(float fValue)
{
  if (fValue == m_fLastValue || m_sParameterName.IsEmpty())
    return;

  // plasma.particles is not a link dependency, so its component is only ever reached through reflection.
  const plRTTI* pParticleType = plRTTI::FindTypeByName("plParticleComponent");
  if (pParticleType == nullptr)
    return;

  const plAbstractProperty* pProperty = pParticleType->FindPropertyByName("Parameters");
  if (pProperty == nullptr || pProperty->GetCategory() != plPropertyCategory::Map)
    return;

  for (plComponent* pComponent : GetOwner()->GetComponents())
  {
    if (pComponent->IsInstanceOf(pParticleType))
    {
      plReflectionUtils::SetMapPropertyValue(static_cast<const plAbstractMapProperty*>(pProperty), pComponent, m_sParameterName.GetData(), plVariant(fValue));
      m_fLastValue = fValue;
      return;
    }
  }
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidDriftSprayComponent);
