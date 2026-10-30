#pragma once

#include <Core/Messages/EventMessage.h>
#include <Foundation/Communication/Message.h>
#include <Foundation/Math/Vec3.h>
#include <FluidPlugin/FluidPluginDLL.h>

/// \brief Pokes a fluid volume from script: one splash, gust or dent at a world position.
///
/// Sent to a game object carrying a plFluidVolumeComponent. The impulse is queued on that component's world module and
/// reaches every volume of that world whose box contains the position, not only the component that received it.
struct PL_FLUIDPLUGIN_DLL plMsgFluidImpulse : public plEventMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgFluidImpulse, plEventMessage);

  /// World space centre of the impulse.
  plVec3 m_vPosition = plVec3::MakeZero();

  /// World space radius in metres over which the impulse falls off.
  float m_fRadius = 0.5f;

  /// World space velocity pushed into the velocity field, metres per second.
  plVec3 m_vVelocity = plVec3::MakeZero();

  /// Strength of the injected density and pressure.
  float m_fAmount = 1.0f;

  /// Push on the surface of a Water volume, metres per second. Negative dents, positive lifts.
  float m_fHeightImpulse = 0.0f;
};
