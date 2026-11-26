using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using ExrCompare.Native;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace ExrCompare;

/// <summary>Main window: wiring between the WinUI controls and the native engine.</summary>
public sealed partial class MainWindow : Window
{
    // Created before InitializeComponent: XAML fires change handlers while it loads.
    private readonly ExrCore _core = new(RamPercent(50));
    private bool _ready;  // false until the constructor has finished wiring everything
    private readonly DispatcherQueueTimer _timer;
    private JsonObject _status = new();
    private JsonObject? _ocio;
    private bool _updating;          // suppresses feedback while code writes control values
    private bool _scrubbing;
    private int _tick;
    private int _lastFrame = int.MinValue;
    private string _lastRenderError = "";
    private readonly List<(long num, long den)> _fpsOptions = new();

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.TitleBar.ButtonBackgroundColor = Colors.Transparent;
        AppWindow.TitleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
        AppWindow.TitleBar.ButtonForegroundColor = Colors.White;
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1760, 1040));

        LayerPanelA.SetSlot(0);
        LayerPanelB.SetSlot(1);
        foreach (var panel in new[] { LayerPanelA, LayerPanelB })
        {
            panel.Send = cmd => Send(cmd);
            panel.Changed = AfterChange;
            panel.OpenRequested = slot => _ = PickAndOpen(slot, folder: false);
            panel.VersionRequested = (slot, delta) => ChangeVersion(slot, delta);
            panel.CloseRequested = CloseLayer;
        }
        TimelineSlider.AddHandler(UIElement.PointerPressedEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler((_, _) => _scrubbing = true), true);
        TimelineSlider.AddHandler(UIElement.PointerReleasedEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler((_, _) => _scrubbing = false), true);
        TimelineSlider.AddHandler(UIElement.PointerCaptureLostEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler((_, _) => _scrubbing = false), true);

        _updating = true;
        CompareBar.SelectedItem = CompareBar.Items[0];
        _updating = false;
        UpdateFpsCombo(24, 1);
        RefreshOcio();
        RefreshLayerPanels();

        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(33);
        _timer.Tick += (_, _) => OnTick();
        _timer.Start();
        Closed += (_, _) =>
        {
            _timer.Stop();
            _core.Dispose();
        };
        _ready = true;
    }

    // ------------------------------------------------------------------ engine --
    private JsonObject Send(object command)
    {
        var reply = _core.Call(command);
        if (!ExrCore.Ok(reply)) ReportError(ExrCore.Error(reply));
        return reply;
    }

    public void ReportError(string message)
    {
        ErrorBar.Message = message;
        ErrorBar.IsOpen = true;
    }

    /// <summary>After anything that changes what is drawn or where.</summary>
    private void AfterChange()
    {
        _status = _core.Call(new { cmd = "status" });
        RefreshGeometry();
        ApplyStatus();
        RefreshSidePanels(force: true);
    }

    private static long RamPercent(int percent)
    {
        long total = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes;
        if (total <= 0) total = 8L << 30;
        return total / 100 * percent;
    }

    // ----------------------------------------------------------------- viewer --
    private double Scale => Viewer.CompositionScaleX > 0 ? Viewer.CompositionScaleX : 1.0;

    private void OnViewerLoaded(object sender, RoutedEventArgs e)
    {
        IntPtr unknown = WinRT.MarshalInspectable<object>.FromManaged(Viewer);
        try
        {
            int result = _core.AttachPanel(unknown, PixelWidth(), PixelHeight(), Viewer.CompositionScaleX, Viewer.CompositionScaleY);
            if (result != 0) ReportError("Display: " + J.Str(_core.Call(new { cmd = "render_status" }), "error"));
        }
        finally
        {
            Marshal.Release(unknown);
        }
    }

    private int PixelWidth() => Math.Max(1, (int)Math.Round(ViewerArea.ActualWidth * Viewer.CompositionScaleX));
    private int PixelHeight() => Math.Max(1, (int)Math.Round(ViewerArea.ActualHeight * Viewer.CompositionScaleY));

    private void OnViewerScaleChanged(SwapChainPanel sender, object args)
    {
        if (!_ready) return;
        _core.Resize(PixelWidth(), PixelHeight(), sender.CompositionScaleX, sender.CompositionScaleY);
        RefreshGeometry();
    }

    private void OnViewerSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (!_ready) return;
        _core.Resize(PixelWidth(), PixelHeight(), Viewer.CompositionScaleX, Viewer.CompositionScaleY);
        // The engine learns the new size on its next frame; re-read geometry shortly after.
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, RefreshGeometry);
        _cacheKey = "";
    }

    // ------------------------------------------------------------------ timer --
    private void OnTick()
    {
        _tick++;
        _status = _core.Call(new { cmd = "status" });
        ApplyStatus();
        if (_tick % 30 == 0)
        {
            var rs = _core.Call(new { cmd = "render_status" });
            string err = J.Str(rs, "error") + J.Str(rs, "shaderError");
            if (err.Length > 0 && err != _lastRenderError) ReportError("Display: " + err);
            _lastRenderError = err;
        }
        if (_tick % 6 == 0) UpdateCacheStrips();
        if (_tick % 5 == 0 && _geometryDirty) RefreshGeometry();
    }

    private void ApplyStatus()
    {
        bool has = J.Bool(_status, "hasContent");
        EmptyState.Visibility = has ? Visibility.Collapsed : Visibility.Visible;
        if (!has)
        {
            HudPanel.Visibility = Visibility.Collapsed;
            return;
        }
        int frame = J.Int(_status, "frame");
        bool playing = J.Bool(_status, "playing");
        int dir = J.Int(_status, "direction", 1);
        _updating = true;
        TimelineSlider.Minimum = J.Int(_status, "first");
        TimelineSlider.Maximum = Math.Max(J.Int(_status, "last"), J.Int(_status, "first") + 1);
        TimelineSlider.IsEnabled = J.Int(_status, "last") > J.Int(_status, "first");
        if (!_scrubbing) TimelineSlider.Value = frame;
        RangeStartText.Text = J.Int(_status, "first").ToString();
        RangeEndText.Text = J.Int(_status, "last").ToString();
        LoopToggle.IsChecked = J.Bool(_status, "loop", true);
        LoopMenuItem.IsChecked = LoopToggle.IsChecked == true;
        string compare = J.Str(_status, "compare", "a");
        var item = CompareBar.Items.FirstOrDefault(i => (string)i.Tag == compare);
        if (item is not null && CompareBar.SelectedItem != item) CompareBar.SelectedItem = item;
        DiffGainBox.Visibility = compare == "difference" ? Visibility.Visible : Visibility.Collapsed;
        WipeSoftBox.Visibility = compare == "wipe" ? Visibility.Visible : Visibility.Collapsed;
        _updating = false;

        CurrentFrameText.Text = frame.ToString();
        PlayIcon.Symbol = playing && dir > 0 ? Symbol.Pause : Symbol.Play;
        ReverseIcon.Glyph = playing && dir < 0 ? "" : "";
        MeasuredFpsText.Text = playing ? $"{J.Num(_status, "measuredFps"):0.00} fps" : "";
        CacheText.Text = $"Cache {J.Num(_status, "cacheBytes") / 1073741824.0:0.0} / {J.Num(_status, "cacheBudget") / 1073741824.0:0.0} GB";

        var layers = _status["layers"] as JsonArray;
        bool ready = true;
        var missing = new List<string>();
        for (int s = 0; s < 2 && layers is not null; s++)
        {
            var l = layers[s];
            if (!J.Bool(l, "loaded")) continue;
            ready &= J.Bool(l, "ready", true);
            if (J.Bool(l, "missing") && LayerShown(s)) missing.Add($"{(s == 0 ? "A" : "B")}: {J.Str(l, "error", "missing")} ({J.Int(l, "frame")})");
        }
        LoadingRing.IsActive = !ready;
        MissingText.Text = string.Join("\n", missing);
        MissingText.Visibility = missing.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

        bool hud = HudMenuItem.IsChecked;
        HudPanel.Visibility = hud ? Visibility.Visible : Visibility.Collapsed;
        if (hud) HudText.Text = BuildHud(layers, frame);

        if (frame != _lastFrame)
        {
            _lastFrame = frame;
            OnFrameChanged(playing);
        }
    }

    private bool LayerShown(int slot)
    {
        string c = J.Str(_status, "compare", "a");
        return c switch { "a" => slot == 0, "b" => slot == 1, _ => true };
    }

    private string BuildHud(JsonArray? layers, int frame)
    {
        var lines = new List<string>();
        var color = _status["color"];
        for (int s = 0; s < 2 && layers is not null; s++)
        {
            var l = layers[s];
            if (!J.Bool(l, "loaded")) continue;
            lines.Add($"{(s == 0 ? "A" : "B")}  {J.Str(l, "pattern")}  [{J.Str(l, "aovName")}]  frame {J.Int(l, "frame")}" +
                      $"{(J.Bool(l, "proxy") ? "  proxy" : "")}{(J.Str(l, "mode") != "color" ? "  " + J.Str(l, "mode") : "")}");
        }
        lines.Add($"frame {frame}   in/out {J.Int(_status, "in")}-{J.Int(_status, "out")}   {J.Str(_status, "fpsLabel")} fps" +
                  $"   {(J.Bool(_status, "playAllFrames") ? "all frames" : "realtime")}   {J.Str(_status, "compare")}");
        lines.Add($"{J.Str(color, "display")} / {J.Str(color, "view")}{(J.Str(color, "look").Length > 0 ? " + " + J.Str(color, "look") : "")}" +
                  $"   exp {J.Num(color, "exposure"):+0.0;-0.0}   gamma {J.Num(color, "gamma"):0.00}");
        return string.Join("\n", lines);
    }

    private void OnFrameChanged(bool playing)
    {
        DrawOverlay();
        LoadNote();
        if (!playing || _tick % 8 == 0) RefreshSidePanels(force: false);
        if (_lastPointer is { } p && !playing) UpdateProbe(p);
    }

    // ------------------------------------------------------------- cache bars --
    private string _cacheKey = "";

    private void UpdateCacheStrips()
    {
        int first = J.Int(_status, "first"), last = J.Int(_status, "last");
        for (int s = 0; s < 2; s++)
        {
            var canvas = s == 0 ? CacheStripA : CacheStripB;
            var layers = _status["layers"] as JsonArray;
            if (layers is null || !J.Bool(layers[s], "loaded"))
            {
                canvas.Children.Clear();
                continue;
            }
            var frames = (_core.Call(new { cmd = "cached", slot = s })["frames"] as JsonArray)?.Select(f => f!.GetValue<int>()).ToList() ?? new();
            string key = $"{s}|{first}|{last}|{canvas.ActualWidth}|{string.Join(",", frames)}";
            if (s == 0 && key == _cacheKey) continue;
            if (s == 0) _cacheKey = key;
            canvas.Children.Clear();
            double width = canvas.ActualWidth;
            int len = last - first + 1;
            if (width <= 0 || len <= 0) continue;
            var brush = (Brush)Application.Current.Resources[s == 0 ? "AccentFillColorDefaultBrush" : "SystemFillColorCautionBrush"];
            for (int i = 0; i < frames.Count;)
            {
                int j = i;
                while (j + 1 < frames.Count && frames[j + 1] == frames[j] + 1) j++;
                double x0 = (frames[i] - first) / (double)len * width, x1 = (frames[j] - first + 1) / (double)len * width;
                var run = new Rectangle { Fill = brush, Height = 3, Width = Math.Max(1, x1 - x0) };
                Canvas.SetLeft(run, x0);
                canvas.Children.Add(run);
                i = j + 1;
            }
        }
    }

    // ------------------------------------------------------------- transport --
    private void OnPlayPause(object sender, RoutedEventArgs e)
    {
        bool forward = J.Bool(_status, "playing") && J.Int(_status, "direction", 1) > 0;
        Send(forward ? new { cmd = "pause" } : (object)new { cmd = "play", direction = 1 });
    }

    private void OnPlayReverse(object sender, RoutedEventArgs e)
    {
        bool reverse = J.Bool(_status, "playing") && J.Int(_status, "direction", 1) < 0;
        Send(reverse ? new { cmd = "pause" } : (object)new { cmd = "play", direction = -1 });
    }

    private void OnStepForward(object sender, RoutedEventArgs e) => Send(new { cmd = "step", delta = 1 });
    private void OnStepBackward(object sender, RoutedEventArgs e) => Send(new { cmd = "step", delta = -1 });
    private void OnGoToStart(object sender, RoutedEventArgs e) => Send(new { cmd = "start" });
    private void OnGoToEnd(object sender, RoutedEventArgs e) => Send(new { cmd = "end" });
    private void OnLoopToggle(object sender, RoutedEventArgs e) => Send(new { cmd = "loop", on = LoopToggle.IsChecked == true });
    private void OnLoopMenu(object sender, RoutedEventArgs e) => Send(new { cmd = "loop", on = LoopMenuItem.IsChecked });
    private void OnPlayAllFrames(object sender, RoutedEventArgs e) => Send(new { cmd = "play_all_frames", on = PlayAllMenuItem.IsChecked });

    private void OnSetIn(object sender, RoutedEventArgs e) =>
        Send(new { cmd = "inout", @in = J.Int(_status, "frame"), @out = J.Int(_status, "out") });

    private void OnSetOut(object sender, RoutedEventArgs e) =>
        Send(new { cmd = "inout", @in = J.Int(_status, "in"), @out = J.Int(_status, "frame") });

    private void OnClearInOut(object sender, RoutedEventArgs e) => Send(new { cmd = "clear_inout" });

    private void OnTimelineChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (!_ready) return;
        if (_updating) return;
        Send(new { cmd = "seek", frame = (int)Math.Round(e.NewValue) });
    }

    private void UpdateFpsCombo(long num, long den)
    {
        _fpsOptions.Clear();
        _fpsOptions.AddRange(new (long, long)[] { (24000, 1001), (24, 1), (25, 1), (30000, 1001), (30, 1), (48, 1), (50, 1), (60000, 1001), (60, 1) });
        if (!_fpsOptions.Any(o => o.num * den == num * o.den)) _fpsOptions.Add((num, den));
        _updating = true;
        FpsCombo.Items.Clear();
        int selected = 0;
        for (int i = 0; i < _fpsOptions.Count; i++)
        {
            var (n, d) = _fpsOptions[i];
            FpsCombo.Items.Add(d == 1 ? $"{n} fps" : $"{(double)n / d:0.###} fps");
            if (n * den == num * d) selected = i;
        }
        FpsCombo.SelectedIndex = selected;
        _updating = false;
    }

    private void OnFpsChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready) return;
        if (_updating || FpsCombo.SelectedIndex < 0) return;
        var (n, d) = _fpsOptions[FpsCombo.SelectedIndex];
        Send(new { cmd = "fps", num = n, den = d });
    }

    private void OnCacheMode(object sender, RoutedEventArgs e)
    {
        Send(new { cmd = "cache", mode = (string)((FrameworkElement)sender).Tag });
        _cacheKey = "";
    }

    private void OnCacheSize(object sender, RoutedEventArgs e) =>
        Send(new { cmd = "cache", budget = RamPercent(int.Parse((string)((FrameworkElement)sender).Tag)) });

    private void SetProxy(bool on)
    {
        ProxyToggle.IsChecked = on;
        ProxyMenuItem.IsChecked = on;
        Send(new { cmd = "proxy", on });
        AfterChange();
    }

    private void OnProxyToggle(object sender, RoutedEventArgs e) => SetProxy(ProxyToggle.IsChecked == true);
    private void OnProxyMenu(object sender, RoutedEventArgs e) => SetProxy(ProxyMenuItem.IsChecked);

    // ---------------------------------------------------------------- compare --
    private void OnCompareBarChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        if (!_ready) return;
        if (_updating || sender.SelectedItem is null) return;
        Send(new { cmd = "compare", mode = (string)sender.SelectedItem.Tag });
        AfterChange();
    }

    private void SetCompare(string mode)
    {
        Send(new { cmd = "compare", mode });
        AfterChange();
    }

    private void OnToggleWipe(object sender, RoutedEventArgs e) => ToggleWipe();
    private void ToggleWipe() => SetCompare(J.Str(_status, "compare") == "wipe" ? "a" : "wipe");

    private void OnSwap(object sender, RoutedEventArgs e)
    {
        Send(new { cmd = "swap" });
        RefreshLayerPanels();
        AfterChange();
    }

    private void OnResetWipe(object sender, RoutedEventArgs e)
    {
        Send(new { cmd = "wipe", reset = true });
        AfterChange();
    }

    private void OnDiffGainChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (!_ready) return;
        if (!_updating && !double.IsNaN(args.NewValue)) Send(new { cmd = "compare", diffGain = args.NewValue });
    }

    private void OnWipeSoftChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (!_ready) return;
        if (!_updating && !double.IsNaN(args.NewValue)) Send(new { cmd = "wipe", softness = args.NewValue * Scale });
    }

    private void SetLockAov(bool on)
    {
        LockAovSwitch.IsOn = on;
        LockAovMenuItem.IsChecked = on;
        Send(new { cmd = "lock_aov", on });
        RefreshLayerPanels();
    }

    private void OnLockAovToggled(object sender, RoutedEventArgs e)
    {
        if (!_ready) return;
        if (!_updating) SetLockAov(LockAovSwitch.IsOn);
    }

    private void OnLockAovMenu(object sender, RoutedEventArgs e) => SetLockAov(LockAovMenuItem.IsChecked);

    private void OnAlign(object sender, RoutedEventArgs e)
    {
        Send(new { cmd = "compare", align = (string)((FrameworkElement)sender).Tag });
        AfterChange();
    }

    // ----------------------------------------------------------------- colour --
    private void OnChannelChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready) return;
        if (!_updating && ChannelCombo.SelectedIndex >= 0) Send(new { cmd = "color", channel = ChannelCombo.SelectedIndex });
    }

    private void SetChannel(int channel) => ChannelCombo.SelectedIndex = channel;

    private void OnExposureChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (!_ready) return;
        ExposureText.Text = e.NewValue.ToString("+0.0;-0.0");
        if (!_updating) Send(new { cmd = "color", exposure = e.NewValue });
    }

    private void OnGammaChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (!_ready) return;
        GammaText.Text = e.NewValue.ToString("0.00");
        if (!_updating) Send(new { cmd = "color", gamma = e.NewValue });
    }

    private void ResetExposure()
    {
        ExposureSlider.Value = 0;
        GammaSlider.Value = 1;
    }

    private void OnResetExposure(object sender, RoutedEventArgs e) => ResetExposure();
    private void OnResetExposureTapped(object sender, Microsoft.UI.Xaml.Input.DoubleTappedRoutedEventArgs e) => ResetExposure();

    private void SendColorFlags()
    {
        Send(new { cmd = "color", falseColor = FalseColorToggle.IsChecked == true, nanHighlight = NanToggle.IsChecked == true, matte = MatteToggle.IsChecked == true });
        FalseColorMenuItem.IsChecked = FalseColorToggle.IsChecked == true;
        NanMenuItem.IsChecked = NanToggle.IsChecked == true;
        MatteMenuItem.IsChecked = MatteToggle.IsChecked == true;
    }

    private void OnColorFlagsToggle(object sender, RoutedEventArgs e) => SendColorFlags();

    private void OnColorFlagsMenu(object sender, RoutedEventArgs e)
    {
        FalseColorToggle.IsChecked = FalseColorMenuItem.IsChecked;
        NanToggle.IsChecked = NanMenuItem.IsChecked;
        MatteToggle.IsChecked = MatteMenuItem.IsChecked;
        SendColorFlags();
    }

    private void ToggleMatte()
    {
        MatteToggle.IsChecked = MatteToggle.IsChecked != true;
        SendColorFlags();
    }

    /// <summary>Re-reads the OCIO config and refreshes the display / view / look lists.</summary>
    private void RefreshOcio()
    {
        _ocio = _core.Call(new { cmd = "ocio" });
        if (!ExrCore.Ok(_ocio)) return;
        _updating = true;
        Fill(DisplayCombo, _ocio["displays"], J.Str(_ocio, "display"));
        Fill(ViewCombo, _ocio["views"], J.Str(_ocio, "view"));
        var looks = (_ocio["looks"] as JsonArray) ?? new JsonArray();
        LookCombo.Visibility = looks.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        LookCombo.Items.Clear();
        LookCombo.Items.Add("(no look)");
        foreach (var l in looks) LookCombo.Items.Add(l!.GetValue<string>());
        string look = J.Str(_ocio, "look");
        LookCombo.SelectedIndex = look.Length == 0 ? 0 : Math.Max(0, LookCombo.Items.IndexOf(look));
        _updating = false;

        BuiltinConfigsMenu.Items.Clear();
        foreach (var b in (_ocio["builtins"] as JsonArray) ?? new JsonArray())
        {
            var item = new MenuFlyoutItem { Text = J.Str(b, "uiName") + (J.Bool(b, "recommended") ? "" : "  (older)"), Tag = J.Str(b, "name") };
            item.Click += (s, _) => LoadOcio((string)((FrameworkElement)s).Tag);
            BuiltinConfigsMenu.Items.Add(item);
        }
        if (BuiltinConfigsMenu.Items.Count == 0)
            BuiltinConfigsMenu.Items.Add(new MenuFlyoutItem { Text = "(none in this OCIO build)", IsEnabled = false });
        string err = J.Str(_ocio, "error");
        if (err.Length > 0) ReportError("OCIO: " + err);
    }

    private static void Fill(ComboBox box, JsonNode? items, string selected)
    {
        box.Items.Clear();
        foreach (var i in (items as JsonArray) ?? new JsonArray()) box.Items.Add(i!.GetValue<string>());
        box.SelectedItem = selected;
    }

    private void LoadOcio(string pathOrUri)
    {
        var r = Send(new { cmd = "ocio_load", path = pathOrUri });
        if (!ExrCore.Ok(r)) return;
        RefreshOcio();
        RefreshLayerPanels();
    }

    private void OnDefaultOcio(object sender, RoutedEventArgs e) => LoadOcio("");

    private void OnDisplayChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready) return;
        if (_updating || DisplayCombo.SelectedItem is not string display) return;
        var r = Send(new { cmd = "color", display });
        _updating = true;
        Fill(ViewCombo, r["views"], J.Str(r["color"], "view"));
        _updating = false;
    }

    private void OnViewChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready) return;
        if (!_updating && ViewCombo.SelectedItem is string view) Send(new { cmd = "color", view });
    }

    private void OnLookChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready) return;
        if (_updating || LookCombo.SelectedIndex < 0) return;
        Send(new { cmd = "color", look = LookCombo.SelectedIndex == 0 ? "" : (string)LookCombo.SelectedItem });
    }

    // ----------------------------------------------------------------- layers --
    private void RefreshLayerPanels()
    {
        var spaces = (_ocio?["colorSpaces"] as JsonArray)?.Select(c => c!.GetValue<string>()).ToList();
        LayerPanelA.Update(_core.Call(new { cmd = "layer", slot = 0 }), spaces);
        LayerPanelB.Update(_core.Call(new { cmd = "layer", slot = 1 }), spaces);
        _updating = true;
        LockAovSwitch.IsOn = J.Bool(_status, "lockAov", true);
        _updating = false;
    }

    private void CloseLayer(int slot)
    {
        Send(new { cmd = "close", slot });
        RefreshLayerPanels();
        AfterChange();
    }

    private void ChangeVersion(int slot, int delta)
    {
        var r = Send(new { cmd = "version", slot, delta });
        if (!ExrCore.Ok(r)) return;
        RefreshLayerPanels();
        AfterChange();
        UpdateTitle();
    }

    private void OnVersionUpA(object sender, RoutedEventArgs e) => ChangeVersion(0, +1);
    private void OnVersionDownA(object sender, RoutedEventArgs e) => ChangeVersion(0, -1);
    private void OnVersionUpB(object sender, RoutedEventArgs e) => ChangeVersion(1, +1);
    private void OnVersionDownB(object sender, RoutedEventArgs e) => ChangeVersion(1, -1);

    private void OnContactSheetA(object sender, RoutedEventArgs e) => LayerPanelA.ToggleContactSheet();

    private void UpdateTitle()
    {
        var names = new List<string>();
        for (int s = 0; s < 2; s++)
        {
            var l = _core.Call(new { cmd = "layer", slot = s });
            if (J.Bool(l, "loaded")) names.Add($"{(s == 0 ? "A" : "B")}: {J.Str(l, "pattern")}");
        }
        TitleText.Text = names.Count == 0 ? "EXR Compare" : "EXR Compare  -  " + string.Join("   ", names);
    }

    // ------------------------------------------------------------------- view --
    private void OnFit(object sender, RoutedEventArgs e) => ViewAction(new { cmd = "view", action = "fit" });
    private void OnOneToOne(object sender, RoutedEventArgs e) => ViewAction(new { cmd = "view", action = "1:1" });

    private void ViewAction(object command)
    {
        ApplyGeometry(Send(command));
    }

    private void ToggleFullScreen()
    {
        AppWindow.SetPresenter(AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen
            ? AppWindowPresenterKind.Default
            : AppWindowPresenterKind.FullScreen);
    }

    private void OnFullScreen(object sender, RoutedEventArgs e) => ToggleFullScreen();

    private void OnHudToggle(object sender, RoutedEventArgs e) => HudMenuItem.IsChecked = HudToggle.IsChecked == true;
    private void OnHudMenu(object sender, RoutedEventArgs e) => HudToggle.IsChecked = HudMenuItem.IsChecked;

    private void SetOverlays(bool on)
    {
        OverlaysToggle.IsChecked = on;
        OverlaysMenuItem.IsChecked = on;
        DrawOverlay();
    }

    private void OnOverlaysToggle(object sender, RoutedEventArgs e) => SetOverlays(OverlaysToggle.IsChecked == true);
    private void OnOverlaysMenu(object sender, RoutedEventArgs e) => SetOverlays(OverlaysMenuItem.IsChecked);

    private void OnSidePaneMenu(object sender, RoutedEventArgs e) =>
        SidePane.Visibility = SidePaneMenuItem.IsChecked ? Visibility.Visible : Visibility.Collapsed;

    private void OnPaneChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        if (!_ready) return;
        string tag = sender.SelectedItem?.Tag as string ?? "layers";
        LayersPage.Visibility = tag == "layers" ? Visibility.Visible : Visibility.Collapsed;
        InfoPage.Visibility = tag == "info" ? Visibility.Visible : Visibility.Collapsed;
        ScopesPage.Visibility = tag == "scopes" ? Visibility.Visible : Visibility.Collapsed;
        RefreshSidePanels(force: true);
    }

    private void OnExit(object sender, RoutedEventArgs e) => Close();
}
