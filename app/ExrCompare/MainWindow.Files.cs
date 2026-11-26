using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using ExrCompare.Native;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace ExrCompare;

/// <summary>Opening sequences, drag and drop, sessions, snapshots, notes export, OCIO config files.</summary>
public sealed partial class MainWindow
{
    private void InitPicker(object picker) =>
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

    private void OnOpenA(object sender, RoutedEventArgs e) => _ = PickAndOpen(0, folder: false);
    private void OnOpenB(object sender, RoutedEventArgs e) => _ = PickAndOpen(1, folder: false);
    private void OnOpenFolderA(object sender, RoutedEventArgs e) => _ = PickAndOpen(0, folder: true);
    private void OnOpenFolderB(object sender, RoutedEventArgs e) => _ = PickAndOpen(1, folder: true);

    private async Task PickAndOpen(int slot, bool folder)
    {
        string? path;
        if (folder)
        {
            var picker = new FolderPicker();
            InitPicker(picker);
            picker.FileTypeFilter.Add("*");
            path = (await picker.PickSingleFolderAsync())?.Path;
        }
        else
        {
            var picker = new FileOpenPicker { ViewMode = PickerViewMode.List };
            InitPicker(picker);
            picker.FileTypeFilter.Add(".exr");
            path = (await picker.PickSingleFileAsync())?.Path;
        }
        if (path is not null) OpenPath(slot, path);
    }

    private void OpenPath(int slot, string path)
    {
        var r = Send(new { cmd = "open", slot, path });
        if (!ExrCore.Ok(r)) return;
        ErrorBar.IsOpen = false;
        if (r["fps"] is JsonArray fps && slot == 0)
            UpdateFpsCombo(fps[0]!.GetValue<long>(), fps[1]!.GetValue<long>());
        RefreshLayerPanels();
        AfterChange();
        UpdateTitle();
        ViewerArea.Focus(FocusState.Programmatic);
    }

    private void OnDragOver(object sender, DragEventArgs e) => e.AcceptedOperation = DataPackageOperation.Copy;

    private async void OnDrop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        var deferral = e.GetDeferral();
        var items = await e.DataView.GetStorageItemsAsync();
        deferral.Complete();
        // First drop fills A; later drops go to B (two items at once: A and B).
        bool aLoaded = LayerPanelA.HasSequence;
        int slot = aLoaded ? 1 : 0;
        foreach (var item in items.Take(2))
        {
            OpenPath(slot, item.Path);
            slot = 1;
        }
    }

    // ----------------------------------------------------------- command line --
    /// <summary>
    /// ExrCompare.exe [pathA] [pathB] [--compare mode] [--snapshot file.png] [--quit]
    /// Paths open into A then B. With --quit the app writes &lt;snapshot&gt;.json (engine and
    /// renderer status, for automated tests) and exits.
    /// </summary>
    private async void HandleCommandLine()
    {
        var args = Environment.GetCommandLineArgs().Skip(1).ToList();
        if (args.Count == 0) return;
        string? compare = null, snapshot = null;
        bool quit = false;
        int slot = 0;
        for (int i = 0; i < args.Count; i++)
        {
            switch (args[i])
            {
                case "--compare" when i + 1 < args.Count: compare = args[++i]; break;
                case "--snapshot" when i + 1 < args.Count: snapshot = args[++i]; break;
                case "--quit": quit = true; break;
                default:
                    if (slot < 2 && (File.Exists(args[i]) || Directory.Exists(args[i]))) OpenPath(slot++, args[i]);
                    break;
            }
        }
        if (compare is not null) SetCompare(compare);
        if (snapshot is null && !quit) return;

        await Task.Delay(2500);  // let the live view draw a few frames
        var report = new JsonObject
        {
            ["status"] = _core.Call(new { cmd = "status" }),
            ["render"] = _core.Call(new { cmd = "render_status" }),
            ["ocio"] = _core.Call(new { cmd = "ocio" }),
        };
        if (snapshot is not null)
        {
            report["snapshot"] = _core.Call(new { cmd = "snapshot", path = snapshot, burnin = new[] { "EXR Compare command-line snapshot" } });
            try { await File.WriteAllTextAsync(snapshot + ".json", report.ToJsonString(new JsonSerializerOptions { WriteIndented = true })); }
            catch (Exception ex) { App.LogError("report", ex); }
        }
        if (quit) Close();
    }

    // ---------------------------------------------------------------- sessions --
    private async void OnSaveSession(object sender, RoutedEventArgs e) => await SaveSession();

    private async Task SaveSession()
    {
        var picker = new FileSavePicker { SuggestedFileName = "review" };
        InitPicker(picker);
        picker.FileTypeChoices.Add("EXR Compare session", new List<string> { ".exrcs" });
        var file = await picker.PickSaveFileAsync();
        if (file is null) return;

        var session = new JsonObject
        {
            ["version"] = 1,
            ["status"] = _core.Call(new { cmd = "status" }),
            ["ocio"] = J.Str(_ocio, "source"),
            ["layers"] = new JsonArray(_core.Call(new { cmd = "layer", slot = 0 }), _core.Call(new { cmd = "layer", slot = 1 })),
            ["notes"] = JsonSerializer.SerializeToNode(_notes.ToDictionary(kv => kv.Key.ToString(), kv => kv.Value)),
        };
        await File.WriteAllTextAsync(file.Path, session.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    private async void OnOpenSession(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        InitPicker(picker);
        picker.FileTypeFilter.Add(".exrcs");
        var file = await picker.PickSingleFileAsync();
        if (file is null) return;
        try
        {
            var s = JsonNode.Parse(await File.ReadAllTextAsync(file.Path))!.AsObject();
            string ocio = J.Str(s, "ocio");
            if (ocio.Length > 0 && ocio != J.Str(_ocio, "source")) LoadOcio(ocio);

            for (int slot = 0; slot < 2; slot++)
            {
                var l = s["layers"]?[slot];
                if (!J.Bool(l, "loaded")) { Send(new { cmd = "close", slot }); continue; }
                if (!ExrCore.Ok(Send(new { cmd = "open", slot, path = J.Str(l, "path") }))) continue;
                Send(new { cmd = "aov", slot, index = J.Int(l, "aov") });
                var set = new JsonObject { ["cmd"] = "layer_set", ["slot"] = slot };
                foreach (var key in new[] { "inputSpace", "mode", "opacity", "visible", "offset", "alpha", "depthAuto", "depthNear",
                                            "depthFar", "depthInvert", "motionScale", "rebuildBeauty", "rebuildAovs", "rebuild" })
                    if (l?[key] is JsonNode v) set[key] = v.DeepClone();
                Send(set.ToJsonString());
            }
            var st = s["status"];
            var color = st?["color"];
            Send(new { cmd = "color", exposure = J.Num(color, "exposure"), gamma = J.Num(color, "gamma", 1), channel = J.Int(color, "channel"),
                       display = J.Str(color, "display"), view = J.Str(color, "view"), look = J.Str(color, "look"),
                       falseColor = J.Bool(color, "falseColor"), nanHighlight = J.Bool(color, "nanHighlight"), matte = J.Bool(color, "matte"),
                       diffGain = J.Num(color, "diffGain", 1) });
            var wipe = st?["wipe"];
            Send(new { cmd = "wipe", x = J.Num(wipe, "x", 0.5), y = J.Num(wipe, "y", 0.5), angle = J.Num(wipe, "angle"),
                       mix = J.Num(wipe, "mix"), softness = J.Num(wipe, "softness") });
            Send(new { cmd = "compare", mode = J.Str(st, "compare", "a"), align = J.Str(st, "align", "fit") });
            Send(new { cmd = "lock_aov", on = J.Bool(st, "lockAov", true) });
            Send(new { cmd = "loop", on = J.Bool(st, "loop", true) });
            if (st?["fps"] is JsonArray fps)
            {
                Send(new { cmd = "fps", num = fps[0]!.GetValue<long>(), den = fps[1]!.GetValue<long>() });
                UpdateFpsCombo(fps[0]!.GetValue<long>(), fps[1]!.GetValue<long>());
            }
            Send(new { cmd = "inout", @in = J.Int(st, "in"), @out = J.Int(st, "out") });
            Send(new { cmd = "seek", frame = J.Int(st, "frame") });

            _notes.Clear();
            if (s["notes"] is JsonObject notes)
                foreach (var kv in notes)
                    _notes[int.Parse(kv.Key)] = kv.Value?.GetValue<string>() ?? "";

            // Bring the toolbar in line with the restored state.
            _updating = true;
            ExposureSlider.Value = J.Num(color, "exposure");
            GammaSlider.Value = J.Num(color, "gamma", 1);
            ChannelCombo.SelectedIndex = J.Int(color, "channel");
            FalseColorToggle.IsChecked = J.Bool(color, "falseColor");
            NanToggle.IsChecked = J.Bool(color, "nanHighlight");
            MatteToggle.IsChecked = J.Bool(color, "matte");
            _updating = false;
            RefreshOcio();
            RefreshLayerPanels();
            AfterChange();
            UpdateTitle();
            LoadNote();
        }
        catch (Exception ex)
        {
            ReportError("Could not open session: " + ex.Message);
        }
    }

    // --------------------------------------------------------------- snapshot --
    private async void OnSnapshot(object sender, RoutedEventArgs e) => await Snapshot();

    private async Task Snapshot()
    {
        if (!J.Bool(_status, "hasContent")) return;
        int frame = J.Int(_status, "frame");
        var picker = new FileSavePicker { SuggestedFileName = $"snapshot.{frame}" };
        InitPicker(picker);
        picker.FileTypeChoices.Add("PNG image", new List<string> { ".png" });
        picker.FileTypeChoices.Add("JPEG image", new List<string> { ".jpg" });
        var file = await picker.PickSaveFileAsync();
        if (file is null) return;

        var layers = _status["layers"] as JsonArray;
        var burnIn = new List<string>();
        for (int s = 0; s < 2 && layers is not null; s++)
            if (J.Bool(layers[s], "loaded") && LayerShown(s))
                burnIn.Add($"{(s == 0 ? "A" : "B")}: {J.Str(layers[s], "pattern")}  [{J.Str(layers[s], "aovName")}]  frame {J.Int(layers[s], "frame")}");
        var color = _status["color"];
        burnIn.Add($"{J.Str(_status, "compare")}   {J.Str(color, "display")} / {J.Str(color, "view")}   exposure {J.Num(color, "exposure"):+0.0;-0.0}");
        if (_notes.TryGetValue(frame, out var note)) burnIn.Add("Note: " + note.Replace('\n', ' '));

        var r = Send(new { cmd = "snapshot", path = file.Path, burnin = burnIn });
        if (ExrCore.Ok(r))
        {
            ErrorBar.Severity = InfoBarSeverity.Success;
            ReportError($"Snapshot saved: {file.Path} ({J.Int(r, "width")} x {J.Int(r, "height")})");
            DispatcherQueue.TryEnqueue(async () =>
            {
                await Task.Delay(4000);
                ErrorBar.IsOpen = false;
                ErrorBar.Severity = InfoBarSeverity.Error;
            });
        }
    }

    // ------------------------------------------------------------------ notes --
    private async void OnExportNotes(object sender, RoutedEventArgs e)
    {
        var picker = new FileSavePicker { SuggestedFileName = "review_notes" };
        InitPicker(picker);
        picker.FileTypeChoices.Add("CSV", new List<string> { ".csv" });
        var file = await picker.PickSaveFileAsync();
        if (file is null) return;
        var layers = _status["layers"] as JsonArray;
        string a = J.Str(layers?[0], "pattern"), b = J.Str(layers?[1], "pattern");
        var sb = new StringBuilder("frame,layer_a,layer_b,note\n");
        foreach (int f in _notes.Keys.OrderBy(f => f))
        {
            string note = _notes[f];
            sb.Append($"{f},\"{a}\",\"{b}\",\"{note.Replace("\"", "\"\"")}\"\n");
        }
        await File.WriteAllTextAsync(file.Path, sb.ToString(), Encoding.UTF8);
    }

    // ------------------------------------------------------------------- OCIO --
    private async void OnLoadOcio(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        InitPicker(picker);
        picker.FileTypeFilter.Add(".ocio");
        picker.FileTypeFilter.Add(".ocioz");
        var file = await picker.PickSingleFileAsync();
        if (file is not null) LoadOcio(file.Path);
    }

    // ------------------------------------------------------------------- help --
    private async void OnShortcuts(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = Content.XamlRoot,
            Title = "Keyboard shortcuts",
            CloseButtonText = "Close",
            Content = new ScrollViewer
            {
                MaxHeight = 560,
                Content = new TextBlock
                {
                    FontFamily = new Microsoft.UI.Xaml.Media.FontFamily("Cascadia Mono, Consolas"),
                    Text =
                        "Space              Play / pause\n" +
                        "Ctrl+Space         Play backward\n" +
                        "Left / Right       Step one frame\n" +
                        "Home / End         First / last frame\n" +
                        "I / O / Shift+I    In point / out point / clear\n" +
                        "Page Up / Down     Previous / next AOV (or mouse wheel over the toolbar AOV menu)\n" +
                        "W                  Toggle wipe\n" +
                        "Enter              Swap A and B\n" +
                        "D / S              Difference / stack\n" +
                        "Alt+Up / Down      Next / previous version of A (add Shift for B)\n" +
                        "C R G B A Y        Colour, red, green, blue, alpha, luminance\n" +
                        "M                  Alpha matte overlay\n" +
                        "0                  Reset exposure and gamma\n" +
                        "F / 1              Fit / actual size (double-click = fit)\n" +
                        "Mouse wheel        Zoom at the pointer;  drag: pan\n" +
                        "Q                  Overlays on / off\n" +
                        "T                  Contact sheet of A's AOVs\n" +
                        "Ctrl+drag          Pixel Analyzer: sample a box (stays until cleared)\n" +
                        "Ctrl+click         Pixel Analyzer: pin one pixel\n" +
                        "Esc                Clear the pixel sample\n" +
                        "Alt+click          Isolate Cryptomatte / ID object\n" +
                        "Ctrl+O / Ctrl+Shift+O   Open into A / B\n" +
                        "Ctrl+E             Export snapshot\n" +
                        "Ctrl+S             Save session\n" +
                        "F11 / Esc          Full screen / leave full screen",
                },
            },
        };
        await dialog.ShowAsync();
    }

    private async void OnAbout(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = Content.XamlRoot,
            Title = "EXR Compare",
            CloseButtonText = "Close",
            Content = $"OpenEXR sequence viewer and comparison tool.\nOCIO {J.Str(_ocio, "version")}, config: {J.Str(_ocio, "name")}\n" +
                      "OpenEXR, OpenColorIO, Direct3D 11 and WinUI 3.",
        };
        await dialog.ShowAsync();
    }
}
