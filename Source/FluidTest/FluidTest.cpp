#include <FluidTest/FluidTestPCH.h>

#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/Strings/StringBuilder.h>
#include <FluidTest/FluidCloudsTest.h>
#include <RendererCore/Textures/TextureUtils.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestSetup.h>

// The game engine test harness ships as sources with the SDK; this executable compiles its own copy.
#include <GameEngineTest/TestClass/TestClass.cpp>

#define PL_FLUIDTEST_STRINGIZE_IMPL(x) #x
#define PL_FLUIDTEST_STRINGIZE(x) PL_FLUIDTEST_STRINGIZE_IMPL(x)

// Named after the engine's game engine tests on purpose: the test project lives in the SDK under
// Data/UnitTests/GameEngineTest, which the test framework derives from this name.
PL_TESTFRAMEWORK_ENTRY_POINT_BEGIN("GameEngineTest", "Fluid Tests")
{
  // The executable lives in the package, so the SDK is not found by walking up from it.
  plFileSystem::SetSdkRootDirectory(PL_FLUIDTEST_STRINGIZE(PL_FLUIDTEST_SDK_ROOT));

  plFluidTestApplication::SetPackageDirectories(PL_FLUIDTEST_STRINGIZE(PL_FLUIDTEST_SDK_BIN_DIR), PL_FLUIDTEST_STRINGIZE(PL_FLUIDTEST_PLUGIN_DIR), PL_FLUIDTEST_STRINGIZE(PL_FLUIDTEST_ASSETS_DIR));

  plTextureUtils::s_bForceFullQualityAlways = true; // never allow to use low-res textures
  plTestFramework::GetInstance()->SetTestTimeout(1000 * 60 * 60);
  plTestFramework::s_bCallstackOnAssert = true;
}
PL_TESTFRAMEWORK_ENTRY_POINT_END()
