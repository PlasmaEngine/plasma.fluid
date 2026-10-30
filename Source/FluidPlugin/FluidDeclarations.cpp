#include <FluidPlugin/FluidPluginPCH.h>

#include <Foundation/IO/Stream.h>
#include <Foundation/Logging/Log.h>
#include <Foundation/Math/Math.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>
#include <FluidPlugin/FluidDeclarations.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidMode, 2)
  PL_ENUM_CONSTANT(plFluidMode::Gas),
  PL_ENUM_CONSTANT(plFluidMode::Water),
  PL_ENUM_CONSTANT(plFluidMode::Snow),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidBoundary, 1)
  PL_ENUM_CONSTANT(plFluidBoundary::Open),
  PL_ENUM_CONSTANT(plFluidBoundary::Closed),
  PL_ENUM_CONSTANT(plFluidBoundary::Wrap),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidAdvection, 1)
  PL_ENUM_CONSTANT(plFluidAdvection::SemiLagrangian),
  PL_ENUM_CONSTANT(plFluidAdvection::MacCormack),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidEmitterShape, 1)
  PL_ENUM_CONSTANT(plFluidEmitterShape::Sphere),
  PL_ENUM_CONSTANT(plFluidEmitterShape::Box),
  PL_ENUM_CONSTANT(plFluidEmitterShape::Cone),
  PL_ENUM_CONSTANT(plFluidEmitterShape::Vortex),
  PL_ENUM_CONSTANT(plFluidEmitterShape::Capsule),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidBodySource, 1)
  PL_ENUM_CONSTANT(plFluidBodySource::Auto),
  PL_ENUM_CONSTANT(plFluidBodySource::Bounds),
  PL_ENUM_CONSTANT(plFluidBodySource::Skeleton),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidColliderShape, 1)
  PL_ENUM_CONSTANT(plFluidColliderShape::Sphere),
  PL_ENUM_CONSTANT(plFluidColliderShape::Box),
  PL_ENUM_CONSTANT(plFluidColliderShape::Capsule),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plFluidDebugView, 1)
  PL_ENUM_CONSTANT(plFluidDebugView::Off),
  PL_ENUM_CONSTANT(plFluidDebugView::Velocity),
  PL_ENUM_CONSTANT(plFluidDebugView::Density),
  PL_ENUM_CONSTANT(plFluidDebugView::Pressure),
  PL_ENUM_CONSTANT(plFluidDebugView::Height),
  PL_ENUM_CONSTANT(plFluidDebugView::Obstacles),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_TYPE(plFluidSettings, plNoBase, 2, plRTTIDefaultAllocator<plFluidSettings>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Mode", plFluidMode, m_Mode)->AddAttributes(new plDefaultValueAttribute(plFluidMode::Gas), new plGroupAttribute("Solver")),
    PL_ENUM_MEMBER_PROPERTY("Boundary", plFluidBoundary, m_Boundary)->AddAttributes(new plDefaultValueAttribute(plFluidBoundary::Open), new plGroupAttribute("Solver")),
    PL_ENUM_MEMBER_PROPERTY("Advection", plFluidAdvection, m_Advection)->AddAttributes(new plDefaultValueAttribute(plFluidAdvection::SemiLagrangian), new plGroupAttribute("Solver")),
    PL_MEMBER_PROPERTY("Resolution", m_uiResolution)->AddAttributes(new plDefaultValueAttribute(256), new plClampValueAttribute(64, 1024), new plGroupAttribute("Solver")),
    PL_MEMBER_PROPERTY("PressureIterations", m_uiPressureIterations)->AddAttributes(new plDefaultValueAttribute(20), new plClampValueAttribute(0, 100), new plGroupAttribute("Solver")),
    PL_MEMBER_PROPERTY("Viscosity", m_fViscosity)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Solver")),

    PL_MEMBER_PROPERTY("DensityDissipation", m_fDensityDissipation)->AddAttributes(new plDefaultValueAttribute(0.98f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Gas")),
    PL_MEMBER_PROPERTY("VelocityDissipation", m_fVelocityDissipation)->AddAttributes(new plDefaultValueAttribute(0.995f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Gas")),
    PL_MEMBER_PROPERTY("Buoyancy", m_fBuoyancy)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(-10.0f, 10.0f), new plGroupAttribute("Gas")),
    PL_MEMBER_PROPERTY("AmbientTemperature", m_fAmbientTemperature)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(-10.0f, 10.0f), new plGroupAttribute("Gas")),
    PL_MEMBER_PROPERTY("Vorticity", m_fVorticity)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Gas")),

    PL_MEMBER_PROPERTY("Depth", m_fDepth)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.05f, 100.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("Damping", m_fDamping)->AddAttributes(new plDefaultValueAttribute(0.004f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("SpongeWidth", m_fSpongeWidth)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("Friction", m_fFriction)->AddAttributes(new plDefaultValueAttribute(0.025f), new plClampValueAttribute(0.0f, 0.2f), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("DryDepth", m_fDryDepth)->AddAttributes(new plDefaultValueAttribute(0.01f), new plClampValueAttribute(0.001f, 0.5f), new plSuffixAttribute(" m"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("WetMemory", m_fWetMemory)->AddAttributes(new plDefaultValueAttribute(10.0f), new plClampValueAttribute(0.1f, 600.0f), new plSuffixAttribute(" sec"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("FoamDivergence", m_fFoamDivergence)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 20.0f), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("FoamSurf", m_fFoamSurf)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 20.0f), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("FoamShear", m_fFoamShear)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 20.0f), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("FoamDecay", m_fFoamDecay)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("RainDropRate", m_fRainDropRate)->AddAttributes(new plDefaultValueAttribute(200.0f), new plClampValueAttribute(0.0f, 10000.0f), new plSuffixAttribute("/sec"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("RainDropSize", m_fRainDropSize)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 10.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("SubSteps", m_uiSubSteps)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 16), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("Chop", m_fChop)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 2.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("ChopScale", m_fChopScale)->AddAttributes(new plDefaultValueAttribute(4.0f), new plClampValueAttribute(0.2f, 100.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Water")),
    PL_MEMBER_PROPERTY("Turbulence", m_fTurbulence)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Water")),

    PL_MEMBER_PROPERTY("AccumulationRate", m_fAccumulationRate)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("MaxDepth", m_fMaxDepth)->AddAttributes(new plDefaultValueAttribute(0.4f), new plClampValueAttribute(0.0f, 10.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("Compression", m_fCompression)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("Ridge", m_fRidge)->AddAttributes(new plDefaultValueAttribute(0.6f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("Slump", m_fSlump)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("Repose", m_fRepose)->AddAttributes(new plDefaultValueAttribute(0.02f), new plClampValueAttribute(0.0f, 1.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("Settle", m_fSettle)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Snow")),
    PL_MEMBER_PROPERTY("RefillRate", m_fRefillRate)->AddAttributes(new plDefaultValueAttribute(0.02f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Snow")),

    PL_MEMBER_PROPERTY("WindStrength", m_fWindStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Coupling")),
    PL_MEMBER_PROPERTY("TouchStrength", m_fTouchStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Coupling")),
    PL_MEMBER_PROPERTY("TouchRadiusScale", m_fTouchRadiusScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 10.0f), new plGroupAttribute("Coupling")),

    PL_MEMBER_PROPERTY("HeightScale", m_fHeightScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Output")),
    PL_MEMBER_PROPERTY("EdgeFade", m_fEdgeFade)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Output")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;
// clang-format on

plFluidMode::Enum plFluidMode::FromStoredValue(plUInt8 uiValue)
{
  switch (uiValue)
  {
    case 0:
      return Gas;
    case 1: // Liquid, before 0.2
    case 3: // Flow, before 0.2
      return Water;
    case 2:
      return Snow;
    default:
      return Default;
  }
}

plUInt32 plFluidSettings::GetClampedResolution() const
{
  const plUInt32 uiClamped = plMath::Clamp(m_uiResolution, 64u, 1024u);

  const plUInt32 uiLower = plMath::PowerOfTwo_Floor(uiClamped);
  const plUInt32 uiUpper = plMath::PowerOfTwo_Ceil(uiClamped);

  return (uiClamped - uiLower <= uiUpper - uiClamped) ? uiLower : uiUpper;
}

void plFluidSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_Mode.GetValue();
  inout_stream << m_Boundary.GetValue();
  inout_stream << m_Advection.GetValue();

  inout_stream << m_uiResolution;
  inout_stream << m_uiPressureIterations;
  inout_stream << m_fViscosity;

  inout_stream << m_fDensityDissipation;
  inout_stream << m_fVelocityDissipation;
  inout_stream << m_fBuoyancy;
  inout_stream << m_fAmbientTemperature;
  inout_stream << m_fVorticity;

  inout_stream << m_fDepth;
  inout_stream << m_fDamping;
  inout_stream << m_fSpongeWidth;
  inout_stream << m_fFriction;
  inout_stream << m_fDryDepth;
  inout_stream << m_fWetMemory;
  inout_stream << m_fFoamDivergence;
  inout_stream << m_fFoamSurf;
  inout_stream << m_fFoamShear;
  inout_stream << m_fFoamDecay;
  inout_stream << m_fRainDropRate;
  inout_stream << m_fRainDropSize;
  inout_stream << m_uiSubSteps;
  inout_stream << m_fChop;
  inout_stream << m_fChopScale;
  inout_stream << m_fTurbulence;

  inout_stream << m_fAccumulationRate;
  inout_stream << m_fMaxDepth;
  inout_stream << m_fCompression;
  inout_stream << m_fRidge;
  inout_stream << m_fSlump;
  inout_stream << m_fRepose;
  inout_stream << m_fSettle;
  inout_stream << m_fRefillRate;

  inout_stream << m_fWindStrength;
  inout_stream << m_fTouchStrength;
  inout_stream << m_fTouchRadiusScale;

  inout_stream << m_fHeightScale;
  inout_stream << m_fEdgeFade;
}

namespace
{
  /// Open water as deep as a wave speed needs: the deep-water speed of shallow water is sqrt(g * depth).
  float DepthFromWaveSpeed(float fWaveSpeed)
  {
    return plMath::Clamp(fWaveSpeed * fWaveSpeed / 9.81f, 0.05f, 100.0f);
  }
} // namespace

plResult plFluidSettings::Load(plStreamReader& inout_stream, plUInt8 uiVersion)
{
  if (uiVersion < 1 || uiVersion > SerializationVersion)
  {
    plLog::Error("Unknown fluid settings version {}", uiVersion);
    return PL_FAILURE;
  }

  // Older layouts carry fewer fields; the rest keep their defaults.
  *this = plFluidSettings();

  plUInt8 uiMode = 0;
  plUInt8 uiBoundary = 0;
  plUInt8 uiAdvection = 0;
  inout_stream >> uiMode;
  inout_stream >> uiBoundary;
  inout_stream >> uiAdvection;

  // Out of range values come from corrupt data and fall back to the enum default.
  m_Mode = plFluidMode::FromStoredValue(uiMode);
  m_Boundary.SetValue(uiBoundary <= plFluidBoundary::Wrap ? uiBoundary : (plUInt8)plFluidBoundary::Default);
  m_Advection.SetValue(uiAdvection <= plFluidAdvection::MacCormack ? uiAdvection : (plUInt8)plFluidAdvection::Default);

  inout_stream >> m_uiResolution;
  inout_stream >> m_uiPressureIterations;
  inout_stream >> m_fViscosity;

  inout_stream >> m_fDensityDissipation;
  inout_stream >> m_fVelocityDissipation;
  inout_stream >> m_fBuoyancy;
  inout_stream >> m_fAmbientTemperature;
  inout_stream >> m_fVorticity;

  if (uiVersion >= 5)
  {
    inout_stream >> m_fDepth;
    inout_stream >> m_fDamping;
    inout_stream >> m_fSpongeWidth;
    inout_stream >> m_fFriction;
    inout_stream >> m_fDryDepth;
    inout_stream >> m_fWetMemory;
    inout_stream >> m_fFoamDivergence;
    inout_stream >> m_fFoamSurf;
    inout_stream >> m_fFoamShear;
    inout_stream >> m_fFoamDecay;
    inout_stream >> m_fRainDropRate;
    inout_stream >> m_fRainDropSize;
    inout_stream >> m_uiSubSteps;

    // Version 6: chop and turbulence.
    if (uiVersion >= 6)
    {
      inout_stream >> m_fChop;
      inout_stream >> m_fChopScale;
      inout_stream >> m_fTurbulence;
    }

    inout_stream >> m_fAccumulationRate;
    inout_stream >> m_fMaxDepth;
    inout_stream >> m_fCompression;
    inout_stream >> m_fRidge;
    inout_stream >> m_fSlump;
    inout_stream >> m_fRepose;
    inout_stream >> m_fSettle;
    inout_stream >> m_fRefillRate;

    inout_stream >> m_fWindStrength;
    inout_stream >> m_fTouchStrength;
    inout_stream >> m_fTouchRadiusScale;

    inout_stream >> m_fHeightScale;
    inout_stream >> m_fEdgeFade;
    return PL_SUCCESS;
  }

  // Versions 1 to 4: the separate Liquid and Flow settings. The Liquid wave speed becomes the open water depth that
  // travels at that speed; the foam threshold and the Flow elevation solve have no counterpart any more.
  float fWaveSpeed = 4.0f;
  float fUnused = 0.0f;
  plUInt32 uiUnused = 0;

  inout_stream >> fWaveSpeed;
  inout_stream >> m_fDamping;
  inout_stream >> m_fRainDropRate;
  inout_stream >> m_fRainDropSize;
  inout_stream >> fUnused; // foam threshold
  inout_stream >> m_fFoamDecay;
  m_fDepth = DepthFromWaveSpeed(fWaveSpeed);

  inout_stream >> m_fAccumulationRate;
  inout_stream >> m_fMaxDepth;
  inout_stream >> m_fCompression;
  inout_stream >> m_fRidge;

  // Version 2: the loose snow layer.
  if (uiVersion >= 2)
  {
    inout_stream >> m_fSlump;
    inout_stream >> m_fRepose;
    inout_stream >> m_fSettle;
  }
  inout_stream >> m_fRefillRate;

  inout_stream >> m_fWindStrength;
  inout_stream >> m_fTouchStrength;
  inout_stream >> m_fTouchRadiusScale;

  inout_stream >> m_fHeightScale;
  inout_stream >> m_fEdgeFade;

  // Version 3: the sponge and the Flow free-surface elevation.
  if (uiVersion >= 3)
  {
    inout_stream >> m_fSpongeWidth;
    inout_stream >> fUnused;  // elevation gain
    inout_stream >> uiUnused; // elevation iterations
  }

  // Version 4: the screening range of the Flow elevation.
  if (uiVersion >= 4)
  {
    inout_stream >> fUnused;
  }

  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// Document patches for the merge of Liquid and Flow into Water (plasma.fluid 0.2).

namespace
{
  /// Rewrites a Mode property holding a retired mode to Water.
  void PatchFluidModeProperty(plAbstractObjectNode* pNode, plStringView sProperty)
  {
    auto* pMode = pNode->FindProperty(sProperty);
    if (pMode == nullptr || !pMode->m_Value.IsA<plString>())
      return;

    const plString& sMode = pMode->m_Value.Get<plString>();
    if (sMode == "plFluidMode::Liquid" || sMode == "plFluidMode::Flow")
    {
      pNode->ChangeProperty(sProperty, plString("plFluidMode::Water"));
    }
  }
} // namespace

class plFluidSettingsPatch_1_2 : public plGraphPatch
{
public:
  plFluidSettingsPatch_1_2()
    : plGraphPatch("plFluidSettings", 2)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    PL_IGNORE_UNUSED(ref_context);
    PL_IGNORE_UNUSED(pGraph);

    PatchFluidModeProperty(pNode, "Mode");

    if (auto* pWaveSpeed = pNode->FindProperty("WaveSpeed"))
    {
      if (pWaveSpeed->m_Value.CanConvertTo<float>())
      {
        const float fDepth = DepthFromWaveSpeed(pWaveSpeed->m_Value.ConvertTo<float>());
        pNode->RemoveProperty("WaveSpeed");
        pNode->AddProperty("Depth", fDepth);
      }
    }

    pNode->RemoveProperty("FoamThreshold");
    pNode->RemoveProperty("FlowElevationGain");
    pNode->RemoveProperty("ElevationIterations");
    pNode->RemoveProperty("ElevationRange");
  }
};

plFluidSettingsPatch_1_2 g_plFluidSettingsPatch_1_2;

class plFluidVolumeComponentPatch_4_5 : public plGraphPatch
{
public:
  plFluidVolumeComponentPatch_4_5()
    : plGraphPatch("plFluidVolumeComponent", 5)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    PL_IGNORE_UNUSED(ref_context);
    PL_IGNORE_UNUSED(pGraph);

    PatchFluidModeProperty(pNode, "Mode");
  }
};

plFluidVolumeComponentPatch_4_5 g_plFluidVolumeComponentPatch_4_5;

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_FluidDeclarations);
