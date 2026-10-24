// Builds the example player from the command line (or JSUI > Build Example):
//
//   Unity -batchmode -nographics -quit -projectPath integrations/unity/project \
//         -executeMethod BuildExample.Build [-out build/unity/JSUI Example.app]
//
// It copies the shared HUD bundle (integrations/shared/ui/dist/bundle.js,
// from `npx jsui build`) into StreamingAssets, creates the example scene if
// it is missing (one GameObject with ExampleGame; the game builds the rest in
// code) and builds a standalone player for the editor's platform.
using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class BuildExample
{
    const string ScenePath = "Assets/Example/Example.unity";
    const string MaterialPath = "Assets/Example/Lit.mat";

    static string RepoRoot => Path.GetFullPath(Path.Combine(Application.dataPath, "..", "..", "..", ".."));

    [MenuItem("JSUI/Copy Bundle + Save Example Scene")]
    public static void Prepare()
    {
        var bundle = Path.Combine(RepoRoot, "integrations", "shared", "ui", "dist", "bundle.js");
        if (!File.Exists(bundle))
            throw new Exception($"{bundle} is missing: run `npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js` first");
        var dest = Path.Combine(Application.streamingAssetsPath, "jsui");
        Directory.CreateDirectory(dest);
        File.Copy(bundle, Path.Combine(dest, "bundle.js"), true);
        AssetDatabase.Refresh();

        var mat = AssetDatabase.LoadAssetAtPath<Material>(MaterialPath);
        if (!mat)
        {
            mat = new Material(Shader.Find("Standard"));
            AssetDatabase.CreateAsset(mat, MaterialPath);
        }

        if (!File.Exists(ScenePath))
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            var game = new GameObject("Game").AddComponent<ExampleGame>();
            game.baseMaterial = mat;
            EditorSceneManager.SaveScene(scene, ScenePath);
        }
        EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(ScenePath, true) };
    }

    [MenuItem("JSUI/Build Example")]
    public static void Build()
    {
        Prepare();

        PlayerSettings.companyName = "JSUI";
        PlayerSettings.productName = "JSUI Example";
        PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
        PlayerSettings.defaultScreenWidth = 1280;
        PlayerSettings.defaultScreenHeight = 720;
        PlayerSettings.resizableWindow = true;
        PlayerSettings.runInBackground = true;
        PlayerSettings.usePlayerLog = true;

        var target = Application.platform switch
        {
            RuntimePlatform.OSXEditor => BuildTarget.StandaloneOSX,
            RuntimePlatform.WindowsEditor => BuildTarget.StandaloneWindows64,
            _ => BuildTarget.StandaloneLinux64,
        };
        var name = target switch
        {
            BuildTarget.StandaloneOSX => "JSUI Example.app",
            BuildTarget.StandaloneWindows64 => "JSUI Example/JSUI Example.exe",
            _ => "JSUI Example/jsui-example",
        };
        var output = Arg("-out") ?? Path.Combine(RepoRoot, "build", "unity", name);
        var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
        {
            scenes = new[] { ScenePath },
            locationPathName = output,
            target = target,
            options = BuildOptions.None,
        });
        var s = report.summary;
        Debug.Log($"BuildExample: {s.result} -> {output} ({s.totalSize / (1024 * 1024)} MB, {s.totalErrors} errors)");
        if (Application.isBatchMode) EditorApplication.Exit(s.result == BuildResult.Succeeded ? 0 : 1);
    }

    static string Arg(string name)
    {
        var args = Environment.GetCommandLineArgs();
        for (int i = 0; i + 1 < args.Length; i++)
            if (args[i] == name) return args[i + 1];
        return null;
    }
}
