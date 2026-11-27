using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json.Nodes;
using ExrCompare.Native;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace ExrCompare;

/// <summary>
/// Waveform monitor overlay. The engine counts every pixel on the GPU (scene-linear or display
/// nits) and draws the traces in a viewer corner; this part places the header, scale labels and
/// settings menu over it, shows the value under the pointer and keeps clicks off the image.
/// </summary>
public sealed partial class MainWindow
{
    private const double WaveHeaderDip = 28;
    private const double WaveBandPx = 3;  // out-of-range bands in the shader (device pixels)

    private Rect? _waveBox;               // whole overlay, DIPs
    private Rect _wavePlot;               // trace area, DIPs
    private string _waveLayoutKey = "";
    private JsonObject? _wave;            // last "waveform" status
    private bool _waveHover;
    private readonly Dictionary<string, MenuFlyoutItemBase> _waveItems = new();

    private static readonly (string group, string title, (string value, string text)[] options)[] WaveGroups =
    {
        ("mode", "Mode", new[] { ("luma", "Luma"), ("parade", "RGB parade"), ("overlay", "RGB overlay") }),
        ("source", "Source", new[] { ("viewer", "Viewer (what's on screen)"), ("a", "Full frame A"), ("b", "Full frame B"),
                                     ("sample", "Sample box (Ctrl+drag)") }),
        ("scale", "Scale", new[] { ("stops6", "±6 stops"), ("stops8", "±8 stops"), ("stops10", "±10 stops"), ("stops14", "±14 stops"),
                                   ("linear1", "Linear 0–1"), ("linear4", "Linear 0–4"), ("linear16", "Linear 0–16") }),
        ("units", "Units", new[] { ("scene", "Scene linear (file values)"), ("nits", "Display light (nits, after the view)") }),
        ("corner", "Corner", new[] { ("br", "Bottom right"), ("bl", "Bottom left"), ("tr", "Top right"), ("tl", "Top left") }),
        ("size", "Size", new[] { ("s", "Small"), ("m", "Medium"), ("l", "Large") }),
    };

    private bool WaveOn => J.Bool(_wave, "enabled");

    private void OnWaveformToggle(object sender, RoutedEventArgs e) => SetWaveform(WaveformToggle.IsChecked == true);
    private void OnWaveformMenu(object sender, RoutedEventArgs e) => SetWaveform(WaveformMenuItem.IsChecked);
    private void OnWaveClose(object sender, RoutedEventArgs e) => SetWaveform(false);
    private void ToggleWaveform() => SetWaveform(!WaveOn);
    private void SetWaveform(bool on) => SendWave(new JsonObject { ["enabled"] = on });

    private void SendWave(JsonObject fields)
    {
        fields["cmd"] = "waveform";
        fields["headerPx"] = (int)Math.Round(WaveHeaderDip * Scale);
        var r = _core.Call(fields.ToJsonString());
        if (!ExrCore.Ok(r)) return;
        ApplyWaveform(r["waveform"] as JsonObject);
    }

    private void BuildWaveMenu()
    {
        var menu = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight };
        foreach (var (group, title, options) in WaveGroups)
        {
            var sub = new MenuFlyoutSubItem { Text = title };
            foreach (var (value, text) in options)
            {
                var item = new RadioMenuFlyoutItem { Text = text, GroupName = "wave-" + group, Tag = value };
                string g = group, v = value;
                item.Click += (_, _) => SendWave(new JsonObject { [g] = v });
                sub.Items.Add(item);
                _waveItems[group + ":" + value] = item;
            }
            menu.Items.Add(sub);
            if (group == "units")
            {
                var exp = new ToggleMenuFlyoutItem { Text = "Include viewer exposure (scene units)" };
                exp.Click += (_, _) => SendWave(new JsonObject { ["exposure"] = exp.IsChecked });
                menu.Items.Add(exp);
                _waveItems["exposure"] = exp;
                menu.Items.Add(new MenuFlyoutSeparator());
            }
        }
        WaveSettingsButton.Flyout = menu;
    }

    /// <summary>Places the header and scale labels over the GPU-drawn waveform (only when it moved).</summary>
    private void ApplyWaveform(JsonObject? w)
    {
        _wave = w;
        bool on = J.Bool(w, "enabled");
        WaveformToggle.IsChecked = on;
        WaveformMenuItem.IsChecked = on;
        if (!on || !J.Bool(_status, "hasContent") || J.Rect(w, "box") is not { } box || J.Rect(w, "plot") is not { } plot)
        {
            WaveLayer.Visibility = Visibility.Collapsed;
            _waveBox = null;
            _waveLayoutKey = "";
            return;
        }
        string key = w!.ToJsonString() + Scale.ToString(CultureInfo.InvariantCulture);
        if (key == _waveLayoutKey) return;
        _waveLayoutKey = key;
        if (_waveItems.Count == 0) BuildWaveMenu();

        double s = Scale;
        _waveBox = new Rect(box[0] / s, box[1] / s, (box[2] - box[0]) / s, (box[3] - box[1]) / s);
        _wavePlot = new Rect(plot[0] / s, plot[1] / s, (plot[2] - plot[0]) / s, (plot[3] - plot[1]) / s);
        WaveLayer.Visibility = Visibility.Visible;
        Canvas.SetLeft(WaveHeader, _waveBox.Value.X);
        Canvas.SetTop(WaveHeader, _waveBox.Value.Y);
        WaveHeader.Width = _waveBox.Value.Width;
        WaveHeader.Height = WaveHeaderDip;

        string mode = J.Str(w, "mode"), units = J.Str(w, "units"), source = J.Str(w, "effectiveSource");
        string modeText = mode switch { "parade" => "RGB parade", "overlay" => "RGB", _ => "Luma" };
        string unitText = units == "nits" ? "nits" : J.Bool(w, "log") ? "stops" : "linear";
        string sourceText = source switch { "a" => "A", "b" => "B", "sample" => "box", _ => "viewer" };
        WaveTitle.Text = $"Waveform  {modeText} · {unitText} · {sourceText}";

        // Scale labels, strongest first, skipping any that would overlap.
        WaveLabels.Children.Clear();
        var placed = new List<double>();
        if (w["labels"] is JsonArray labels)
        {
            foreach (var l in labels.OrderByDescending(l => J.Num(l, "strength")))
            {
                double y = J.Num(l, "y") / s;
                if (placed.Any(p => Math.Abs(p - y) < 12)) continue;
                placed.Add(y);
                var t = new TextBlock
                {
                    Text = J.Str(l, "text"), FontSize = 10.5, FontFamily = Mono, IsHitTestVisible = false,
                    Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 190, 190, 190)),
                    Opacity = Math.Clamp(0.45 + J.Num(l, "strength") * 0.6, 0.5, 1.0),
                };
                Canvas.SetLeft(t, _wavePlot.X + 4);
                Canvas.SetTop(t, y - 13);
                WaveLabels.Children.Add(t);
            }
        }

        foreach (var (group, _, options) in WaveGroups)
            foreach (var (value, _) in options)
                if (_waveItems.TryGetValue(group + ":" + value, out var item) && item is RadioMenuFlyoutItem r)
                    r.IsChecked = J.Str(w, group) == value;
        if (_waveItems.TryGetValue("exposure", out var e) && e is ToggleMenuFlyoutItem ex) ex.IsChecked = J.Bool(w, "exposure");
        if (!_waveHover) UpdateWaveStats();
    }

    private bool InWave(Point p) => _waveBox is { } b && WaveLayer.Visibility == Visibility.Visible && b.Contains(p);

    /// <summary>Value under the pointer, from the same scale the engine draws.</summary>
    private void UpdateWaveHover(Point p)
    {
        if (_wave is null || !_wavePlot.Contains(p))
        {
            ClearWaveHover();
            return;
        }
        _waveHover = true;
        double band = WaveBandPx / Scale;
        double inner = _wavePlot.Height - 2 * band;
        double t = (_wavePlot.Bottom - band - p.Y) / inner;
        if (t > 1) { WaveReadout.Text = "above scale"; return; }
        if (t < 0) { WaveReadout.Text = J.Bool(_wave, "log") ? "below scale / zero / negative" : "below scale"; return; }
        double lo = J.Num(_wave, "lo"), hi = J.Num(_wave, "hi");
        double v = lo + t * (hi - lo);
        if (J.Bool(_wave, "log")) v = Math.Pow(2, v);
        WaveReadout.Text = FormatWaveValue(v);
    }

    private void ClearWaveHover()
    {
        if (!_waveHover) return;
        _waveHover = false;
        UpdateWaveStats();
    }

    private string FormatWaveValue(double v)
    {
        var ci = CultureInfo.InvariantCulture;
        if (J.Str(_wave, "units") == "nits") return v.ToString(v >= 100 ? "0" : v >= 1 ? "0.0" : "0.000", ci) + " nits";
        string lin = v.ToString(v >= 100 ? "0.0" : v >= 1 ? "0.000" : "0.0000", ci);
        if (v <= 0) return lin;
        double stops = Math.Log2(v / 0.18);
        return $"{stops.ToString("+0.0;-0.0", ci)} st · {lin}";
    }

    /// <summary>Header text when not hovering: frame peak (in the scale's units) and NaN/Inf count.</summary>
    private void UpdateWaveStats()
    {
        if (!WaveOn || _waveHover) return;
        var r = _core.Call(new { cmd = "waveform_stats" });
        double peak = J.Num(r, "peak");
        int nan = J.Int(r, "nan");
        string err = J.Str(r, "error");
        WaveReadout.Text = err.Length > 0 ? "shader error (see Help)"
                         : (peak > 0 ? "peak " + FormatWaveValue(peak) : "") + (nan > 0 ? $"   NaN/Inf {nan}" : "");
    }
}
