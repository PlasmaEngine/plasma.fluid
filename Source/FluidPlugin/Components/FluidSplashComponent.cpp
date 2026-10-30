#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/Messages/CommonMessages.h>
#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <FluidPlugin/Components/FluidSplashComponent.h>
#include <FluidPlugin/Components/FluidVolumeComponent.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Reflection/ReflectionUtils.h>

plCVarBool cvar_FluidDebugLogSplash("Fluid.Debug.LogSplash", false, plCVarFlags::Default, "Logs every splash a Fluid Splash component starts");

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plFluidSplashComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("RiseThreshold", m_fRiseThreshold)->AddAttributes(new plClampValueAttribute(0.01f, plVariant()), new plDefaultValueAttribute(0.25f)),
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Cooldown", m_fCooldown)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Reach", m_fReach)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(2.0f)),
    PL_MEMBER_PROPERTY("LaunchScale", m_fLaunchScale)->AddAttributes(new plClampValueAttribute(0.0f, plVariant()), new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("ParameterName", GetParameterName, SetParameterName)->AddAttributes(new plDefaultValueAttribute("Splash")),
    PL_ACCESSOR_PROPERTY("SpeedParameterName", GetSpeedParameterName, SetSpeedParameterName)->AddAttributes(new plDefaultValueAttribute("Speed")),
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

plFluidSplashComponent::plFluidSplashComponent()
{
  m_sParameterName.Assign("Splash");
  m_sSpeedParameterName.Assign("Speed");
}

plFluidSplashComponent::~plFluidSplashComponent() = default;

void plFluidSplashComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_fRiseThreshold;
  s << m_fStrength;
  s << m_fCooldown;
  s << m_fReach;
  s << m_fLaunchScale;
  s << m_sParameterName;
  s << m_sSpeedParameterName;
}

void plFluidSplashComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_fRiseThreshold;
  s >> m_fStrength;
  s >> m_fCooldown;
  s >> m_fReach;
  s >> m_fLaunchScale;
  s >> m_sParameterName;
  s >> m_sSpeedParameterName;
}

void plFluidSplashComponent::OnDeactivated()
{
  m_uiLastReadback = 0;
  m_NextSplashTime = plTime::MakeZero();

  SUPER::OnDeactivated();
}

void plFluidSplashComponent::Update()
{
  // The first Water volume among the ancestors.
  const plFluidVolumeComponent* pVolume = nullptr;
  for (const plGameObject* pObject = GetOwner()->GetParent(); pObject != nullptr; pObject = pObject->GetParent())
  {
    const plFluidVolumeComponent* pCandidate = nullptr;
    if (pObject->TryGetComponentOfBaseType(pCandidate) && pCandidate->IsActiveAndInitialized() && pCandidate->GetEffectiveSettings().m_Mode == plFluidMode::Water)
    {
      pVolume = pCandidate;
      break;
    }
  }

  if (pVolume == nullptr)
    return;

  // The solver ignores the volume's scale, so this does too.
  plTransform volumeTransform = pVolume->GetOwner()->GetGlobalTransform();
  volumeTransform.m_vScale.Set(1.0f);
  const plVec3 vLocal = volumeTransform.GetInverse().TransformPosition(GetOwner()->GetGlobalPosition());
  const plVec2 vExtents = pVolume->GetExtents();
  const plVec2 vUv(vLocal.x / plMath::Max(vExtents.x, 0.01f) + 0.5f, vLocal.y / plMath::Max(vExtents.y, 0.01f) + 0.5f);
  const plVec2 vReachUv(m_fReach / plMath::Max(vExtents.x, 0.01f), m_fReach / plMath::Max(vExtents.y, 0.01f));

  plVec4 cell;
  plUInt32 uiReadback = 0;
  if (!plFluidGpuManager::GetWaterActivityAround(GetWorld()->GetIndex(), pVolume->GetVolumeHandle(), vUv, vReachUv, cell, uiReadback) || uiReadback == m_uiLastReadback)
    return;

  const plTime now = GetWorld()->GetClock().GetAccumulatedTime();

  if (m_uiLastReadback != 0 && now >= m_NextSplashTime)
  {
    const float fElapsed = static_cast<float>((now - m_LastReadbackTime).GetSeconds());
    const float fRise = fElapsed > 0.0f ? (cell.y - m_fLastHighest) / fElapsed : 0.0f;
    const float fThreshold = plMath::Max(m_fRiseThreshold, 0.01f);

    if (fRise > fThreshold)
    {
      // Any hit throws up a fair splash; a harder one up to the full burst. The water leaves at the speed it arrived
      // with plus what falling from the wave's height would give it.
      const float fWaveHeight = plMath::Max(cell.y, 0.05f);
      const float fSpeed = (cell.z + plMath::Sqrt(2.0f * 9.81f * fWaveHeight)) * m_fLaunchScale;
      Splash((0.4f + 0.6f * plMath::Saturate((fRise - fThreshold) / fThreshold)) * m_fStrength, fSpeed);
      m_NextSplashTime = now + plTime::MakeFromSeconds(m_fCooldown);

      if (cvar_FluidDebugLogSplash)
      {
        plLog::Info("Fluid: splash '{}' at {} s, water rising {} m/s, wave {} m, flow {} m/s, launch {} m/s", GetOwner()->GetName(), plArgF(now.GetSeconds(), 2),
          plArgF(fRise, 2), plArgF(cell.y, 2), plArgF(cell.z, 2), plArgF(fSpeed, 2));
      }
    }
  }

  m_fLastHighest = cell.y;
  m_uiLastReadback = uiReadback;
  m_LastReadbackTime = now;
}

void plFluidSplashComponent::Splash(float fStrength, float fSpeed)
{
  // plasma.particles is not a link dependency: the parameters go in through reflection, the start through a message.
  SetEffectParameter(m_sParameterName, fStrength);
  SetEffectParameter(m_sSpeedParameterName, fSpeed);

  plMsgSetPlaying msg;
  msg.m_bPlay = true;
  GetOwner()->SendMessage(msg);
}

void plFluidSplashComponent::SetEffectParameter(const plHashedString& sName, float fValue)
{
  if (sName.IsEmpty())
    return;

  const plRTTI* pParticleType = plRTTI::FindTypeByName("plParticleComponent");
  const plAbstractProperty* pProperty = pParticleType != nullptr ? pParticleType->FindPropertyByName("Parameters") : nullptr;
  if (pProperty == nullptr || pProperty->GetCategory() != plPropertyCategory::Map)
    return;

  for (plComponent* pComponent : GetOwner()->GetComponents())
  {
    if (pComponent->IsInstanceOf(pParticleType))
    {
      plReflectionUtils::SetMapPropertyValue(static_cast<const plAbstractMapProperty*>(pProperty), pComponent, sName.GetData(), plVariant(fValue));
    }
  }
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Components_FluidSplashComponent);
