// Windows players: puts the ICU data next to the player's copy of the plugin.
//
// On Windows, Skia's ICU (text shaping, line breaking) reads icudtl.dat from
// beside the executable or the DLL that contains it (SkLoadICU). The package
// ships it next to Plugins/Windows/x86_64/jsui_unity.dll, which is where the
// Editor loads the plugin from, but a player build copies only the DLL
// (into <Product>_Data/Plugins/x86_64/); this copies the data file beside it.
// Without it SkUnicode is unavailable and text shaping fails.
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Jsui.Editor
{
    class JsuiBuildPostprocess : IPostprocessBuildWithReport
    {
        public int callbackOrder => 0;

        public void OnPostprocessBuild(BuildReport report)
        {
            if (report.summary.platform != BuildTarget.StandaloneWindows64) return;
            var icu = FindIcuData();
            if (icu == null)
            {
                Debug.LogWarning("JSUI: icudtl.dat not found next to Plugins/Windows/x86_64/jsui_unity.dll; text shaping will fail in the player");
                return;
            }
            var exe = report.summary.outputPath;
            var data = Path.Combine(Path.GetDirectoryName(exe), Path.GetFileNameWithoutExtension(exe) + "_Data");
            var copied = false;
            if (Directory.Exists(data))
            {
                foreach (var dll in Directory.GetFiles(data, "jsui_unity.dll", SearchOption.AllDirectories))
                {
                    File.Copy(icu, Path.Combine(Path.GetDirectoryName(dll), "icudtl.dat"), true);
                    copied = true;
                }
            }
            // Next to the executable works too (SkLoadICU looks there first).
            if (!copied) File.Copy(icu, Path.Combine(Path.GetDirectoryName(exe), "icudtl.dat"), true);
        }

        static string FindIcuData()
        {
            // The package's folder (Packages/... resolves to wherever Package
            // Manager put it), then anywhere in the project's assets.
            var p = Path.GetFullPath("Packages/com.orktes.jsui/Plugins/Windows/x86_64/icudtl.dat");
            if (File.Exists(p)) return p;
            foreach (var guid in AssetDatabase.FindAssets("icudtl"))
            {
                var path = AssetDatabase.GUIDToAssetPath(guid);
                if (path.EndsWith("icudtl.dat") && File.Exists(Path.GetFullPath(path))) return Path.GetFullPath(path);
            }
            return null;
        }
    }
}
