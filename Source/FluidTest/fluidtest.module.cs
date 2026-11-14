using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class FluidTestModule : ModuleRules
{
    public FluidTestModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/FluidTest", "FluidTestPCH.h",
            "BUILDSYSTEM_BUILDING_FLUIDTEST");

        // Test framework headers and import library from the SDK. The game engine test harness
        // (GameEngineTest/TestClass) is compiled into this executable from the SDK's sources.
        var unitTestsRoot = Path.Combine(PlasmaPackageSdk.Root, "Code", "UnitTests");
        PublicIncludePaths.Add(unitTestsRoot);
        PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests", "TestFramework"));

        // TestFrameworkEntryPoint_Platform.h
        var platformFolder = context.Platform == TargetPlatform.Windows ? "Win"
            : context.Platform == TargetPlatform.Mac ? "OSX"
            : context.Platform == TargetPlatform.Linux ? "Linux"
            : null;
        if (platformFolder != null)
        {
            PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework", "Platform", platformFolder));
        }

        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/FluidPlugin", "plFluidPlugin");

        // The test data lives in the SDK and the fluid solver shaders in this package's Assets, which the
        // test mounts as the data directory an installed package gets. Passed unquoted (the command line drops the
        // quotes) and stringized in FluidTest.cpp, so these paths must not contain spaces.
        PrivateDefinitions.Add($"PL_FLUIDTEST_SDK_ROOT={PlasmaPackageSdk.Root.Replace('\\', '/')}");
        PrivateDefinitions.Add($"PL_FLUIDTEST_PLUGIN_DIR={PlasmaPackageSdk.PackageBinaryDirectory(context).Replace('\\', '/')}");
        PrivateDefinitions.Add($"PL_FLUIDTEST_SDK_BIN_DIR={PlasmaPackageSdk.SdkBinaryDirectory(context).Replace('\\', '/')}");
        PrivateDefinitions.Add($"PL_FLUIDTEST_ASSETS_DIR={Path.Combine(context.ProjectRoot, "Assets").Replace('\\', '/')}");
    }
}
