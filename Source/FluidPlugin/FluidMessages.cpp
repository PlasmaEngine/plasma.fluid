#include <FluidPlugin/FluidPluginPCH.h>

#include <FluidPlugin/FluidMessages.h>

// clang-format off
PL_IMPLEMENT_MESSAGE_TYPE(plMsgFluidImpulse)
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgFluidImpulse, 1, plRTTIDefaultAllocator<plMsgFluidImpulse>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Position", m_vPosition),
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.01f, 100.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Velocity", m_vVelocity),
    PL_MEMBER_PROPERTY("Amount", m_fAmount)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("HeightImpulse", m_fHeightImpulse),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_FluidMessages);
