#pragma once

#include <Core/Graphics/Camera.h>
#include <GameEngineTest/TestClass/TestClass.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/ViewRenderMode.h>

class plFluidEmitterComponent;
class plVolumetricCloudsComponent;

/// The game state of the fluid tests. Keeps the project's main render pipeline, which has the volumetric clouds pass.
class plFluidTestGameState : public plGameEngineTestGameState
{
  PL_ADD_DYNAMIC_REFLECTION(plFluidTestGameState, plGameEngineTestGameState);
};

/// Base application of the fluid tests: this package's plugin on the plugin search path and its Assets mounted as
/// the data directory an installed package gets ("plasma.fluid"), so the solver shaders resolve.
class plFluidTestApplication : public plGameEngineTestApplication
{
public:
  explicit plFluidTestApplication(const char* szProjectDirName);

  /// \brief Where the SDK binaries, this package's plugin binaries and its Assets are. Set once by the entry point.
  static void SetPackageDirectories(plStringView sSdkBinaryDirectory, plStringView sPluginDirectory, plStringView sAssetsDirectory);

protected:
  virtual void Init_AddPluginSearchRoots() override;
  virtual void Init_FileSystem_ConfigureDataDirs() override;
};

/// Fluid-driven clouds: a Gas volume over the sky is the volumetric clouds' weather map. Every clip is a scene built in
/// code and a fixed sequence of frames, so two runs give the same video.
class plFluidCloudsApplication : public plFluidTestApplication
{
public:
  plFluidCloudsApplication();

  enum class Clip
  {
    HurricaneOrbit, ///< a storm seen from above while the camera circles it
    HurricaneEye,   ///< standing in the clear eye, looking around at the eyewall
    BeamGround,     ///< under an overcast deck, a beam punches a hole and the sun pours through
    BeamAerial,     ///< the same from above the deck
    Count
  };

  struct ClipInfo
  {
    const char* m_szName = "";    ///< folder under the output directory
    const char* m_szTitle = "";   ///< caption of the video
    plInt32 m_iFrames = 0;        ///< frames written, played back at 30 per second
    plInt32 m_iWarmupFrames = 0;  ///< frames rendered before the first one written
    float m_fWarmupStep = 0.0f;   ///< fluid seconds per warm-up frame: spins the storm up quickly
    float m_fStep = 0.0f;         ///< fluid seconds per written frame: the time-lapse
  };

  static const ClipInfo& GetClipInfo(Clip clip);

  plResult SetupClip(Clip clip);

  /// \brief Animation of a clip: camera, sun and emitters. iFrame < 0 during warm-up.
  void SetFrame(Clip clip, plInt32 iFrame, float fFluidSeconds);

  plResult SaveFrame(Clip clip, plInt32 iFrame);
  plResult WriteTitle(Clip clip);

protected:
  virtual plUniquePtr<plGameStateBase> CreateGameState() override;
  virtual void BeforeHighLevelSystemsShutdown() override;

private:
  struct WeatherSetup
  {
    float m_fLayerMin = 1000.0f;
    float m_fLayerMax = 4000.0f;
    float m_fExtent = 40000.0f;
    plUInt32 m_uiResolution = 1024;
    float m_fAmbientDensity = 0.6f;
    float m_fAmbientRate = 0.0f;
    float m_fAmbientScale = 4000.0f;
    float m_fAmbientBreakup = 0.5f;
    float m_fVorticity = 0.0f;
    float m_fVelocityDissipation = 0.999f;
    float m_fTemperatureDissipation = 0.9f;
    plUInt32 m_uiPressureIterations = 40;
    float m_fFlowPeriod = 25.0f; ///< fluid seconds per cycle of the clouds' flow-mapped noise
  };

  void AddSunAndSky(float fLux, bool bShadows);
  void SetSun(float fElevationDegrees, float fAzimuthDegrees);
  void SetCamera(const plVec3& vPos, const plVec3& vTarget, float fFovY);
  plGameObject* AddBox(const char* szName, const plVec3& vCenter, const plVec3& vSize, const plColor& color, bool bFullbright = false, bool bCastShadow = true);
  plVolumetricCloudsComponent* AddClouds(const WeatherSetup& weather);
  void AddWeather(const WeatherSetup& weather);
  plFluidEmitterComponent* AddEmitter(const char* szName, const plVec2& vPos);

  void SetupHurricane();
  void SetupBeam();

  plMeshResourceHandle m_hBoxMesh;
  plMeshResourceHandle m_hGlowBoxMesh;
  plGameObjectHandle m_hBeamColumn; ///< Beam clips: the column of light itself
  plGameObjectHandle m_hSun;
  float m_fWeatherHeight = 0.0f;
  plComponentHandle m_hBeam;      ///< Beam clips: the outflow at the beam's foot in the deck
  plComponentHandle m_hBeamClear; ///< Beam clips: the clearing inside the beam
  plComponentHandle m_hBeamRing;  ///< Beam clips: the wall of cloud the blast piles up
};

class plGameEngineTestFluidClouds : public plGameEngineTest
{
  using SUPER = plGameEngineTest;

public:
  virtual const char* GetTestName() const override;
  virtual plGameEngineTestApplication* CreateApplication() override;

protected:
  virtual void SetupSubTests() override;
  virtual plResult InitializeSubTest(plInt32 iIdentifier) override;
  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override;

private:
  plFluidCloudsApplication* m_pOwnApplication = nullptr;
  plInt32 m_iClipFrame = 0;
  plInt32 m_iFramesToWrite = 0;
  plInt32 m_iSaveEvery = 1;
  double m_fFluidSeconds = 0.0;
};
