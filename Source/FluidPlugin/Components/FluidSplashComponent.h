#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <Core/World/World.h>
#include <Foundation/Strings/HashedString.h>
#include <FluidPlugin/FluidPluginDLL.h>

using plFluidSplashComponentManager = plComponentManagerSimple<class plFluidSplashComponent, plComponentUpdateType::WhenSimulating>;

/// \brief Plays a particle burst each time a wave hits: put it on a rock, a pier post or anything else standing in a
/// Water volume, with the splash effect on the same object.
///
/// The component reads the Water volume's activity within Reach of its owner (plFluidGpuManager::GetWaterActivityAround):
/// when the highest water there rises faster than Rise Threshold, a wave front is arriving and the effect is started
/// once, then not again for Cooldown seconds. The volume is the first Water volume among the owner's ancestors.
///
/// The effect is a plParticleComponent on the same object, started with plMsgSetPlaying and otherwise reached through
/// reflection only. Before each start its Parameters map receives, under ParameterName, how hard the wave hit (up to
/// Strength), which a burst emitter scales its count by; and under SpeedParameterName the speed the water is thrown up
/// at, in m/s: the fastest flow around the owner plus the speed a fall from the wave's height gives, times Launch Scale.
/// A velocity initializer scales its speed by it.
class PL_FLUIDPLUGIN_DLL plFluidSplashComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidSplashComponent, plComponent, plFluidSplashComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidSplashComponent

public:
  plFluidSplashComponent();
  ~plFluidSplashComponent();

  /// \brief Metres per second the highest water in the cell has to rise by to count as a wave hitting.
  float m_fRiseThreshold = 0.25f; // [ property ]

  /// \brief The largest burst, reached at twice the threshold; any hit gets at least 40 % of it.
  float m_fStrength = 1.0f; // [ property ]

  /// \brief Seconds after a splash before the next can start; a wave front takes a moment to pass.
  float m_fCooldown = 1.0f; // [ property ]

  /// \brief Metres around the owner the rising water is looked for in. Larger than the rock: the water under a
  /// collider does not move.
  float m_fReach = 2.0f; // [ property ]

  /// \brief Multiplies the speed the water is thrown up at.
  float m_fLaunchScale = 1.0f; // [ property ]

  /// \brief Name of the effect's float parameter that receives the burst.
  const char* GetParameterName() const { return m_sParameterName.GetData(); }    // [ property ]
  void SetParameterName(const char* szName) { m_sParameterName.Assign(szName); } // [ property ]

  /// \brief Name of the effect's float parameter that receives the launch speed, m/s.
  const char* GetSpeedParameterName() const { return m_sSpeedParameterName.GetData(); }    // [ property ]
  void SetSpeedParameterName(const char* szName) { m_sSpeedParameterName.Assign(szName); } // [ property ]

protected:
  void Update();
  void Splash(float fStrength, float fSpeed);
  void SetEffectParameter(const plHashedString& sName, float fValue);

  plHashedString m_sParameterName;
  plHashedString m_sSpeedParameterName;
  float m_fLastHighest = 0.0f;       ///< highest surface around the owner at the last readback
  plUInt32 m_uiLastReadback = 0;     ///< readback the last value came from; 0 before the first
  plTime m_LastReadbackTime;         ///< world time the last readback was seen
  plTime m_NextSplashTime;           ///< world time before which no splash starts
};
