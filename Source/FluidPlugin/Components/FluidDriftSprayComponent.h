#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <Core/World/World.h>
#include <Foundation/Strings/HashedString.h>
#include <FluidPlugin/FluidPluginDLL.h>

using plFluidDriftSprayComponentManager = plComponentManagerSimple<class plFluidDriftSprayComponent, plComponentUpdateType::WhenSimulating>;

/// \brief Places a particle effect where loose snow is moving in the Snow volume on the parent object, and drives
/// one of its float parameters with how much snow is moving.
///
/// Every frame the component reads the volume's activity grid (plFluidGpuManager::GetSnowActivity), picks a cell with
/// a probability proportional to the snow moving in it, and moves its owner there, Height above the volume plane.
/// Its owner's +X is turned downwind so a velocity cone blows the spray with the wind.
///
/// The effect is a plParticleComponent on the same object. It is found and driven through reflection only: the
/// parameter named ParameterName of its Parameters map receives saturate(total activity / (Threshold * 256)) *
/// Strength. Without a particle component the owner still moves, and nothing else happens.
class PL_FLUIDPLUGIN_DLL plFluidDriftSprayComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidDriftSprayComponent, plComponent, plFluidDriftSprayComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidDriftSprayComponent

public:
  plFluidDriftSprayComponent();
  ~plFluidDriftSprayComponent();

  /// \brief Metres of loose snow moved per step, averaged over the volume, that count as full activity.
  float m_fThreshold = 0.002f; // [ property ]

  /// \brief Multiplies the parameter value the effect receives.
  float m_fStrength = 1.0f; // [ property ]

  /// \brief Name of the effect's float parameter that receives the activity.
  const char* GetParameterName() const { return m_sParameterName.GetData(); }    // [ property ]
  void SetParameterName(const char* szName) { m_sParameterName.Assign(szName); } // [ property ]

  /// \brief Metres above the volume plane the emitter is placed at.
  float m_fHeight = 0.1f; // [ property ]

protected:
  void Update();

  void SetEffectParameter(float fValue);

  plHashedString m_sParameterName;
  float m_fLastValue = -1.0f; ///< last value handed to the effect; -1 forces the next write
};
