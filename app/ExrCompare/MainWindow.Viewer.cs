using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json.Nodes;
using ExrCompare.Native;
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.System;

namespace ExrCompare;

/// <summary>
/// Viewer overlays and pointer handling: wipe widget, data-window outlines, contact-sheet labels and
/// the Pixel Analyzer (hover readout, pinned red sample box).
/// </summary>
public sealed partial class MainWindow
{
    // Geometry from the engine, in device pixels.
    private double[]? _rectA, _rectB, _dataA, _dataB, _regionRect;
    private JsonArray? _tilesA, _tilesB;
    private bool _geometryDirty;

    private enum Drag { None, Pan, WipeCenter, WipeRotate, WipeMix, Region, Click }
    private Drag _drag;
    private Point _dragStart, _dragLast;
    private Point? _lastPointer;

    // Wipe state mirrored locally so dragging never waits on a status round trip.
    private double _wipeX = 0.5, _wipeY = 0.5, _wipeAngle, _wipeMix;

    private const double HandleRadius = 110;  // rotate handle distance from the wipe centre (DIPs)
    private const double ArcRadius = 62;      // mix arc radius
    private const double ArcSpan = Math.PI / 2;

    // Persistent overlay elements (moved, never recreated, while dragging).
    private Canvas? _staticLayer, _wipeLayer;
    private CompositeTransform? _wipeTransform;
    private Ellipse? _wipeMixDot;
    private TextBlock? _wipeLabelA, _wipeLabelB;
    private Rectangle? _dragBox, _sampleBox;

    private static readonly Brush SampleRed = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 40, 40));

    // ------------------------------------------------------------ geometry --
    private void RefreshGeometry() => ApplyGeometry(_core.Call(new { cmd = "geometry" }));

    private void ApplyGeometry(JsonObject g)
    {
        if (!_ready) return;
        if (!ExrCore.Ok(g)) return;
        _rectA = J.Rect(g, "a");
        _rectB = J.Rect(g, "b");
        _dataA = J.Rect(g, "dataWindowA");
        _dataB = J.Rect(g, "dataWindowB");
        _regionRect = J.Rect(g, "region");
        _tilesA = g["tilesA"] as JsonArray;
        _tilesB = g["tilesB"] as JsonArray;
        // The engine learns the viewport size on its next frame; re-check soon if it differs.
        var vp = g["viewport"] as JsonArray;
        _geometryDirty = vp is null || vp[0]!.GetValue<int>() != PixelWidth() || vp[1]!.GetValue<int>() != PixelHeight();
        DrawOverlay();
    }

    private Point ToDip(double x, double y) => new(x / Scale, y / Scale);
    private (double x, double y) ToPx(Point p) => (p.X * Scale, p.Y * Scale);

    // -------------------------------------------------------------- overlay --
    private bool WipeVisible =>
        J.Bool(_status, "hasContent") && OverlaysToggle.IsChecked == true && J.Str(_status, "compare") == "wipe";

    private Point WipeCentre => new(_wipeX * ViewerArea.ActualWidth, _wipeY * ViewerArea.ActualHeight);

    /// <summary>Takes the wipe from the engine status unless the user is dragging it.</summary>
    private void SyncWipeFromStatus()
    {
        if (_drag is Drag.WipeCenter or Drag.WipeRotate or Drag.WipeMix) return;
        var w = _status["wipe"];
        double x = J.Num(w, "x", 0.5), y = J.Num(w, "y", 0.5), a = J.Num(w, "angle"), m = J.Num(w, "mix");
        if (x == _wipeX && y == _wipeY && a == _wipeAngle && m == _wipeMix) return;
        (_wipeX, _wipeY, _wipeAngle, _wipeMix) = (x, y, a, m);
        UpdateWipeWidget();
    }

    private void EnsureOverlay()
    {
        if (_staticLayer is not null) return;
        var white = new SolidColorBrush(Colors.White);
        var shadow = new SolidColorBrush(Windows.UI.Color.FromArgb(160, 0, 0, 0));
        var accent = (Brush)Application.Current.Resources["AccentFillColorDefaultBrush"];
        var caution = (Brush)Application.Current.Resources["SystemFillColorCautionBrush"];

        _staticLayer = new Canvas();
        // Nuke-style wipe widget, drawn once at the origin for angle 0 and moved with a transform.
        _wipeTransform = new CompositeTransform();
        _wipeLayer = new Canvas { RenderTransform = _wipeTransform, Visibility = Visibility.Collapsed };
        void Line(double x0, double y0, double x1, double y1, Brush b, double t) =>
            _wipeLayer.Children.Add(new Line { X1 = x0, Y1 = y0, X2 = x1, Y2 = y1, Stroke = b, StrokeThickness = t });
        Ellipse Dot(double cx, double cy, double r, Brush? fill)
        {
            var e = new Ellipse { Width = r * 2, Height = r * 2, Stroke = white, StrokeThickness = 1.5, Fill = fill };
            Canvas.SetLeft(e, cx - r);
            Canvas.SetTop(e, cy - r);
            _wipeLayer.Children.Add(e);
            return e;
        }
        const double L = 8000;
        Line(0, -L, 0, L, shadow, 3);
        Line(0, -L, 0, L, white, 1);
        Line(0, 0, HandleRadius, 0, white, 1.5);
        Dot(HandleRadius, 0, 6, accent);
        var arc = new PathFigure { StartPoint = Polar(new Point(), ArcRadius, -ArcSpan / 2) };
        arc.Segments.Add(new ArcSegment
        {
            Point = Polar(new Point(), ArcRadius, ArcSpan / 2), Size = new Size(ArcRadius, ArcRadius),
            SweepDirection = SweepDirection.Clockwise, IsLargeArc = false,
        });
        var geo = new PathGeometry();
        geo.Figures.Add(arc);
        _wipeLayer.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = geo, Stroke = white, StrokeThickness = 1.5 });
        _wipeMixDot = Dot(0, 0, 5, caution);
        Dot(0, 0, 8, null);
        Line(-14, 0, 14, 0, white, 1);
        Line(0, -14, 0, 14, white, 1);
        _wipeLabelA = new TextBlock { Text = "A", Foreground = accent, FontWeight = FontWeights.SemiBold, Visibility = Visibility.Collapsed };
        _wipeLabelB = new TextBlock { Text = "B", Foreground = caution, FontWeight = FontWeights.SemiBold, Visibility = Visibility.Collapsed };

        _dragBox = new Rectangle { Stroke = SampleRed, StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 2 }, Visibility = Visibility.Collapsed };
        _sampleBox = new Rectangle { Stroke = SampleRed, StrokeThickness = 1.5, Visibility = Visibility.Collapsed };

        Overlay.Children.Add(_staticLayer);
        Overlay.Children.Add(_sampleBox);
        Overlay.Children.Add(_dragBox);
        Overlay.Children.Add(_wipeLayer);
        Overlay.Children.Add(_wipeLabelA);
        Overlay.Children.Add(_wipeLabelB);
    }

    /// <summary>Moves the wipe widget. Only transform and position properties change: cheap per move.</summary>
    private void UpdateWipeWidget()
    {
        EnsureOverlay();
        var vis = WipeVisible ? Visibility.Visible : Visibility.Collapsed;
        _wipeLayer!.Visibility = _wipeLabelA!.Visibility = _wipeLabelB!.Visibility = vis;
        if (vis == Visibility.Collapsed) return;
        var c = WipeCentre;
        _wipeTransform!.TranslateX = c.X;
        _wipeTransform.TranslateY = c.Y;
        _wipeTransform.Rotation = _wipeAngle * 180 / Math.PI;
        var m = Polar(new Point(), ArcRadius, -ArcSpan / 2 + _wipeMix * ArcSpan);
        Canvas.SetLeft(_wipeMixDot!, m.X - 5);
        Canvas.SetTop(_wipeMixDot!, m.Y - 5);
        var la = Polar(c, 26, _wipeAngle + Math.PI);
        var lb = Polar(c, 26, _wipeAngle);
        Canvas.SetLeft(_wipeLabelA, la.X - 4);
        Canvas.SetTop(_wipeLabelA, la.Y - 10);
        Canvas.SetLeft(_wipeLabelB, lb.X - 4);
        Canvas.SetTop(_wipeLabelB, lb.Y - 10);
    }

    private void UpdateSampleBox()
    {
        EnsureOverlay();
        if (_regionRect is not { } r || OverlaysToggle.IsChecked != true || !J.Bool(_status, "hasContent"))
        {
            _sampleBox!.Visibility = Visibility.Collapsed;
            return;
        }
        var a = ToDip(r[0], r[1]);
        var b = ToDip(r[2], r[3]);
        // At least a few DIPs so a single pinned pixel stays visible when zoomed out.
        double w = Math.Max(3, b.X - a.X), h = Math.Max(3, b.Y - a.Y);
        _sampleBox!.Width = w;
        _sampleBox.Height = h;
        Canvas.SetLeft(_sampleBox, (a.X + b.X - w) / 2);
        Canvas.SetTop(_sampleBox, (a.Y + b.Y - h) / 2);
        _sampleBox.Visibility = Visibility.Visible;
    }

    private void DrawOverlay()
    {
        if (!_ready) return;
        EnsureOverlay();
        _staticLayer!.Children.Clear();
        UpdateWipeWidget();
        UpdateSampleBox();
        if (!J.Bool(_status, "hasContent") || OverlaysToggle.IsChecked != true) return;
        var accent = (Brush)Application.Current.Resources["AccentFillColorDefaultBrush"];
        var caution = (Brush)Application.Current.Resources["SystemFillColorCautionBrush"];
        var white = new SolidColorBrush(Colors.White);
        var shadow = new SolidColorBrush(Windows.UI.Color.FromArgb(160, 0, 0, 0));

        if (DataWindowMenuItem.IsChecked)
        {
            AddRect(_dataA, accent, dashed: true);
            AddRect(_dataB, caution, dashed: true);
        }

        // Contact sheet labels.
        foreach (var tiles in new[] { _tilesA, _tilesB })
        {
            if (tiles is null) continue;
            foreach (var t in tiles)
            {
                var r = J.Rect(t, "rect");
                if (r is null) continue;
                var p = ToDip(r[0], r[1]);
                var label = new Border
                {
                    Background = shadow, Padding = new Thickness(6, 2, 6, 2), CornerRadius = new CornerRadius(3),
                    Child = new TextBlock { Text = J.Str(t, "name"), FontSize = 12, Foreground = white },
                };
                Canvas.SetLeft(label, p.X + 4);
                Canvas.SetTop(label, p.Y + 4);
                _staticLayer.Children.Add(label);
                AddRect(r, new SolidColorBrush(Windows.UI.Color.FromArgb(90, 255, 255, 255)), dashed: false);
            }
        }
    }

    private static Point Polar(Point c, double r, double angle) => new(c.X + Math.Cos(angle) * r, c.Y + Math.Sin(angle) * r);

    private void AddRect(double[]? r, Brush brush, bool dashed)
    {
        if (r is null) return;
        var a = ToDip(r[0], r[1]);
        var b = ToDip(r[2], r[3]);
        var rect = new Rectangle { Width = Math.Max(0, b.X - a.X), Height = Math.Max(0, b.Y - a.Y), Stroke = brush, StrokeThickness = 1 };
        if (dashed) rect.StrokeDashArray = new DoubleCollection { 4, 3 };
        Canvas.SetLeft(rect, a.X);
        Canvas.SetTop(rect, a.Y);
        _staticLayer!.Children.Add(rect);
    }

    // -------------------------------------------------------------- pointer --
    private static bool IsKeyDown(VirtualKey key) =>
        (InputKeyboardSource.GetKeyStateForCurrentThread(key) & Windows.UI.Core.CoreVirtualKeyStates.Down) != 0;

    private void OnViewerPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        ViewerArea.Focus(FocusState.Pointer);
        var pt = e.GetCurrentPoint(ViewerArea);
        var p = pt.Position;
        _dragStart = _dragLast = p;
        _drag = Drag.None;
        if (!J.Bool(_status, "hasContent")) return;
        bool left = pt.Properties.IsLeftButtonPressed, middle = pt.Properties.IsMiddleButtonPressed;
        if (middle) _drag = Drag.Pan;
        else if (left && IsKeyDown(VirtualKey.Menu)) { IsolateAt(p); return; }
        else if (left && IsKeyDown(VirtualKey.Control)) _drag = Drag.Region;
        else if (left && WipeVisible && HitWipe(p) is { } hit) _drag = hit;
        else if (left) _drag = Drag.Click;  // becomes a pan once it moves
        if (_drag != Drag.None) ViewerArea.CapturePointer(e.Pointer);
    }

    private Drag? HitWipe(Point p)
    {
        var c = WipeCentre;
        double Dist(Point q) => Math.Sqrt((p.X - q.X) * (p.X - q.X) + (p.Y - q.Y) * (p.Y - q.Y));
        if (Dist(Polar(c, HandleRadius, _wipeAngle)) < 14) return Drag.WipeRotate;
        if (Dist(Polar(c, ArcRadius, _wipeAngle - ArcSpan / 2 + _wipeMix * ArcSpan)) < 14) return Drag.WipeMix;
        if (Dist(c) < 16) return Drag.WipeCenter;
        return null;
    }

    private void OnViewerPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        var p = e.GetCurrentPoint(ViewerArea).Position;
        _lastPointer = p;
        double dx = p.X - _dragLast.X, dy = p.Y - _dragLast.Y;
        switch (_drag)
        {
            case Drag.Click:
                if (Math.Abs(p.X - _dragStart.X) + Math.Abs(p.Y - _dragStart.Y) > 4) _drag = Drag.Pan;
                break;
            case Drag.Pan:
                ApplyGeometry(_core.Call(new { cmd = "view", action = "pan", dx = dx * Scale, dy = dy * Scale }));
                break;
            case Drag.WipeCenter:
                _wipeX = Math.Clamp(p.X / ViewerArea.ActualWidth, 0, 1);
                _wipeY = Math.Clamp(p.Y / ViewerArea.ActualHeight, 0, 1);
                _core.Call(new { cmd = "wipe", x = _wipeX, y = _wipeY });
                UpdateWipeWidget();
                break;
            case Drag.WipeRotate:
            {
                var c = WipeCentre;
                _wipeAngle = Math.Atan2(p.Y - c.Y, p.X - c.X);
                _core.Call(new { cmd = "wipe", angle = _wipeAngle });
                UpdateWipeWidget();
                break;
            }
            case Drag.WipeMix:
            {
                var c = WipeCentre;
                double rel = Math.IEEERemainder(Math.Atan2(p.Y - c.Y, p.X - c.X) - _wipeAngle, 2 * Math.PI);
                _wipeMix = Math.Clamp((rel + ArcSpan / 2) / ArcSpan, 0, 1);
                _core.Call(new { cmd = "wipe", mix = _wipeMix });
                UpdateWipeWidget();
                break;
            }
            case Drag.Region:
                EnsureOverlay();
                _dragBox!.Width = Math.Abs(p.X - _dragStart.X);
                _dragBox.Height = Math.Abs(p.Y - _dragStart.Y);
                Canvas.SetLeft(_dragBox, Math.Min(_dragStart.X, p.X));
                Canvas.SetTop(_dragBox, Math.Min(_dragStart.Y, p.Y));
                _dragBox.Visibility = Visibility.Visible;
                break;
        }
        _dragLast = p;
        // Hover readout only when not dragging a handle or panning (keeps those moves light).
        if (_drag is Drag.None or Drag.Click or Drag.Region) UpdateProbe(p);
    }

    private void OnViewerPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        var p = e.GetCurrentPoint(ViewerArea).Position;
        switch (_drag)
        {
            case Drag.Click:
                ClickTile(p);
                break;
            case Drag.Region:
            {
                _dragBox!.Visibility = Visibility.Collapsed;
                var (x0, y0) = ToPx(_dragStart);
                var (x1, y1) = ToPx(p);
                // A Ctrl+click (no real box) pins the single pixel under the pointer.
                bool click = Math.Abs(p.X - _dragStart.X) < 3 && Math.Abs(p.Y - _dragStart.Y) < 3;
                if (click) _core.Call(new { cmd = "sample_pixel", x = x0, y = y0 });
                else _core.Call(new { cmd = "sample_region", x0, y0, x1, y1 });
                RefreshGeometry();
                RefreshAnalyzer();
                ShowPane("pixel");
                break;
            }
            case Drag.WipeCenter or Drag.WipeRotate or Drag.WipeMix:
                _status = _core.Call(new { cmd = "status" });
                break;
        }
        _drag = Drag.None;
        ViewerArea.ReleasePointerCapture(e.Pointer);
        UpdateProbe(p);
    }

    private void OnViewerPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (_drag != Drag.None) return;
        _lastPointer = null;
        RefreshAnalyzer();  // readout falls back to the pinned sample (or clears)
    }

    private void OnViewerWheel(object sender, PointerRoutedEventArgs e)
    {
        if (!J.Bool(_status, "hasContent")) return;
        var pt = e.GetCurrentPoint(ViewerArea);
        double factor = Math.Pow(1.1, pt.Properties.MouseWheelDelta / 120.0);
        var (x, y) = ToPx(pt.Position);
        ViewAction(new { cmd = "view", action = "zoom", factor, x, y });
        e.Handled = true;
    }

    private void OnViewerDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (IsKeyDown(VirtualKey.Control)) return;  // Ctrl+click samples, it doesn't refit
        ViewAction(new { cmd = "view", action = "fit" });
    }

    private void ClickTile(Point p)
    {
        var (x, y) = ToPx(p);
        for (int s = 0; s < 2; s++)
        {
            var tiles = s == 0 ? _tilesA : _tilesB;
            if (tiles is null) continue;
            foreach (var t in tiles)
            {
                var r = J.Rect(t, "rect");
                if (r is null || x < r[0] || x >= r[2] || y < r[1] || y >= r[3]) continue;
                Send(new { cmd = "aov", slot = s, index = J.Int(t, "aov") });
                (s == 0 ? LayerPanelA : LayerPanelB).Update(Send(new { cmd = "layer_set", slot = s, contactSheet = false }));
                AfterChange();
                return;
            }
        }
    }

    /// <summary>Alt+click on a Cryptomatte / ID AOV isolates the object under the pointer.</summary>
    private void IsolateAt(Point p)
    {
        var (x, y) = ToPx(p);
        var probe = _core.Call(new { cmd = "probe", x, y });
        for (int s = 0; s < 2; s++)
        {
            var v = probe[s == 0 ? "a" : "b"];
            if (v is null) continue;
            uint? bits = null;
            string label = "";
            if (v["crypto"] is JsonArray ranks && ranks.Count > 0)
            {
                var best = ranks.OrderByDescending(r => J.Num(r, "coverage")).First();
                bits = (uint)J.Num(best, "bits");
                label = J.Str(best, "name");
                if (label.Length == 0) label = J.Str(best, "id");
            }
            else if (v["rgba"] is JsonArray rgba && rgba[0] is JsonValue jv && jv.TryGetValue(out double d))
            {
                bits = BitConverter.SingleToUInt32Bits((float)d);
                label = $"id {d}";
            }
            if (bits is null) continue;
            var panel = s == 0 ? LayerPanelA : LayerPanelB;
            panel.Update(Send(new { cmd = "layer_set", slot = s, isolate = bits.Value }));
            panel.ShowIsolate("Isolated: " + label);
        }
    }

    // -------------------------------------------------------- pixel analyzer --
    private JsonObject? _analyzer;  // last "analyzer" reply with region stats

    private void UpdateProbe(Point p)
    {
        if (!_ready || !J.Bool(_status, "hasContent")) return;
        var (x, y) = ToPx(p);
        var hover = _core.Call(new { cmd = "analyzer", x, y, region = false });
        ShowReadout(hover, hovering: true);
        UpdateAnalyzerCurrent(hover);
        if (PixelPage.Visibility == Visibility.Visible) UpdateCryptoDetail(x, y);
    }

    /// <summary>Re-reads the pinned sample (frame, AOV or layer changed) and refreshes the readouts.</summary>
    private void RefreshAnalyzer()
    {
        if (!_ready) return;
        if (!J.Bool(_status, "hasContent"))
        {
            _analyzer = null;
            ShowReadout(null, hovering: false);
            return;
        }
        JsonObject r;
        if (_lastPointer is { } p)
        {
            var (x, y) = ToPx(p);
            r = _core.Call(new { cmd = "analyzer", x, y });
        }
        else r = _core.Call(new { cmd = "analyzer" });
        _analyzer = r;
        ShowReadout(r, hovering: _lastPointer is not null);
        UpdateAnalyzerTable(r);
    }

    private void OnClearSample(object sender, RoutedEventArgs e) => ClearSample();

    private bool SampleActive => J.Bool(_analyzer, "regionActive");

    private void ClearSample()
    {
        _core.Call(new { cmd = "clear_region" });
        _regionRect = null;
        UpdateSampleBox();
        RefreshAnalyzer();
    }

    // Nuke viewer info bar, one fixed row per input:
    // "A:  5952x3140  bbox -4 -4 5960 3148  channels: rgba      x=3040 y=536 4x4  r g b a [swatch]  H: S: V:  L:"
    private sealed class ReadoutRow
    {
        public Grid Root = null!;
        public TextBlock Info = null!, Pos = null!, Hsv = null!;
        public TextBlock[] Values = new TextBlock[4];
        public Border Swatch = null!;
    }

    private readonly ReadoutRow[] _readout = new ReadoutRow[2];
    private readonly string[] _readoutInfo = { "", "" };

    private static readonly Windows.UI.Color[] ChannelColors =
    {
        Windows.UI.Color.FromArgb(255, 255, 70, 70),
        Windows.UI.Color.FromArgb(255, 60, 220, 90),
        Windows.UI.Color.FromArgb(255, 80, 150, 255),
        Windows.UI.Color.FromArgb(255, 235, 235, 235),
    };

    private static readonly FontFamily Mono = new("Cascadia Mono, Consolas");

    private void BuildReadout()
    {
        var grey = Windows.UI.Color.FromArgb(255, 215, 215, 215);
        for (int s = 0; s < 2; s++)
        {
            var row = new ReadoutRow { Root = new Grid { ColumnSpacing = 14 } };
            // label | info (fills) | position + size | R | G | B | A | swatch | HSV L
            foreach (var w in new[] { 26.0, -1, 200, 86, 86, 86, 86, 40, 330 })
                row.Root.ColumnDefinitions.Add(new ColumnDefinition { Width = w < 0 ? new GridLength(1, GridUnitType.Star) : new GridLength(w) });
            TextBlock T(int col, Windows.UI.Color color, bool right = false)
            {
                var t = new TextBlock
                {
                    FontFamily = Mono, FontSize = 15, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.Clip,
                    TextAlignment = right ? TextAlignment.Right : TextAlignment.Left, Foreground = new SolidColorBrush(color),
                };
                Grid.SetColumn(t, col);
                row.Root.Children.Add(t);
                return t;
            }
            T(0, grey).Text = s == 0 ? "A:" : "B:";
            row.Info = T(1, grey);
            row.Pos = T(2, grey);
            for (int c = 0; c < 4; c++) row.Values[c] = T(3 + c, ChannelColors[c], right: true);
            row.Swatch = new Border { Width = 40, Height = 18, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(row.Swatch, 7);
            row.Root.Children.Add(row.Swatch);
            row.Hsv = T(8, grey);
            Grid.SetRow(row.Root, s);
            _readout[s] = row;
            ReadoutRows.Children.Add(row.Root);
        }
    }

    /// <summary>Left part of a readout row: resolution, data window and channels of the shown AOV.</summary>
    private void SetReadoutInfo(int slot, JsonObject layer)
    {
        if (!J.Bool(layer, "loaded"))
        {
            _readoutInfo[slot] = "";
        }
        else
        {
            var dw = layer["dataWindow"] as JsonArray;
            string bbox = dw is { Count: 4 } ? $"  bbox {dw[0]} {dw[1]} {dw[2]} {dw[3]}" : "";
            var aovs = layer["aovs"] as JsonArray;
            int aov = J.Int(layer, "aov");
            string ch = aovs is not null && aov >= 0 && aov < aovs.Count ? J.Str(aovs[aov], "channels") : "";
            _readoutInfo[slot] = $"{J.Int(layer, "width")}x{J.Int(layer, "height")}{bbox}{(ch.Length > 0 ? "  channels: " + ch : "")}";
        }
        if (_readout[0] is null) BuildReadout();
        _readout[slot].Info.Text = _readoutInfo[slot];
    }

    private void ShowReadout(JsonObject? r, bool hovering)
    {
        if (_readout[0] is null) BuildReadout();
        bool region = J.Bool(r, "regionActive");
        ClearSampleButton.Visibility = region ? Visibility.Visible : Visibility.Collapsed;
        for (int s = 0; s < 2; s++)
        {
            var row = _readout[s];
            row.Info.Text = _readoutInfo[s];
            var l = r?[s == 0 ? "a" : "b"];
            var px = hovering ? l?["pixel"] : null;
            var reg = l?["region"];
            if (px is not null)
            {
                row.Pos.Text = $"x={J.Int(px, "x")} y={J.Int(px, "y")}";
                SetValues(row, px["rgba"] as JsonArray, px["display"] as JsonArray);
            }
            else if (reg is not null)
            {
                row.Pos.Text = $"x={J.Int(reg, "x")} y={J.Int(reg, "y")} {J.Int(reg, "w")}x{J.Int(reg, "h")}";
                SetValues(row, reg["mean"] as JsonArray, reg["display"] as JsonArray);
            }
            else
            {
                // Keep the row (fixed height); just blank the values.
                row.Pos.Text = "";
                SetValues(row, null, null);
            }
        }
    }

    private static void SetValues(ReadoutRow row, JsonArray? v, JsonArray? display)
    {
        for (int c = 0; c < 4; c++) row.Values[c].Text = v is null ? "" : Fmt(v[c]);
        row.Swatch.Background = SwatchBrush(display);
        row.Hsv.Text = v is null ? "" : HsvL(v);
    }

    /// <summary>Nuke's readout tail: HSV of the linear RGB, and Rec.709 luminance.</summary>
    private static string HsvL(JsonArray v)
    {
        double Get(int i) => v[i] is JsonValue jv && jv.TryGetValue(out double d) ? d : double.NaN;
        double r = Get(0), g = Get(1), b = Get(2);
        if (double.IsNaN(r) || double.IsNaN(g) || double.IsNaN(b)) return "";
        double max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b)), d = max - min;
        double h = 0;
        if (d > 0)
        {
            if (max == r) h = 60 * (((g - b) / d) % 6);
            else if (max == g) h = 60 * ((b - r) / d + 2);
            else h = 60 * ((r - g) / d + 4);
            if (h < 0) h += 360;
        }
        double sat = max > 0 ? d / max : 0;
        double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        var ci = CultureInfo.InvariantCulture;
        return $"H:{h.ToString("0", ci)} S:{sat.ToString("0.00", ci)} V:{max.ToString("0.00", ci)}  L: {lum.ToString("0.00000", ci)}";
    }

    /// <summary>Scene-linear values, unclamped, five decimals like Nuke's viewer.</summary>
    private static string Fmt(JsonNode? v)
    {
        if (v is not JsonValue jv) return "-";
        if (jv.TryGetValue(out string? s)) return s ?? "-";
        double d = jv.GetValue<double>();
        return Math.Abs(d) >= 100000 ? d.ToString("0.###e+0", CultureInfo.InvariantCulture) : d.ToString("0.00000", CultureInfo.InvariantCulture);
    }

    private static Brush SwatchBrush(JsonArray? display)
    {
        if (display is null || display.Count < 3) return new SolidColorBrush(Colors.Transparent);
        byte B(int i) => (byte)Math.Round(Math.Clamp(display[i]!.GetValue<double>(), 0, 1) * 255);
        return new SolidColorBrush(Windows.UI.Color.FromArgb(255, B(0), B(1), B(2)));
    }

    // Side-pane table: Current / Min / Max / Average / Median x R G B A.
    private static readonly string[] AnalyzerRows = { "Current", "Min", "Max", "Average", "Median" };
    private static readonly string[] AnalyzerKeys = { "", "min", "max", "mean", "median" };
    private readonly TextBlock[][,] _analyzerCells = new TextBlock[2][,];
    private readonly TextBlock[] _analyzerTitles = new TextBlock[2];
    private readonly Border[][] _analyzerSwatches = new Border[2][];
    private readonly StackPanel[] _analyzerBlocks = new StackPanel[2];

    private void BuildAnalyzerTable()
    {
        for (int s = 0; s < 2; s++)
        {
            var block = new StackPanel { Spacing = 6 };
            _analyzerTitles[s] = new TextBlock { FontFamily = Mono, FontSize = 13, TextWrapping = TextWrapping.Wrap };
            block.Children.Add(_analyzerTitles[s]);
            var grid = new Grid { ColumnSpacing = 6, RowSpacing = 4 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(62) });
            for (int c = 0; c < 4; c++) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(26) });
            for (int r = 0; r <= AnalyzerRows.Length; r++) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            string[] head = { "", "R", "G", "B", "A" };
            for (int c = 1; c < 5; c++)
            {
                var h = new TextBlock { Text = head[c], FontFamily = Mono, FontSize = 13, TextAlignment = TextAlignment.Right, Foreground = new SolidColorBrush(ChannelColors[c - 1]) };
                Grid.SetColumn(h, c);
                grid.Children.Add(h);
            }
            var cells = new TextBlock[AnalyzerRows.Length, 4];
            var swatches = new Border[AnalyzerRows.Length];
            for (int r = 0; r < AnalyzerRows.Length; r++)
            {
                var label = new TextBlock { Text = AnalyzerRows[r], FontSize = 13, Opacity = 0.8 };
                Grid.SetRow(label, r + 1);
                grid.Children.Add(label);
                for (int c = 0; c < 4; c++)
                {
                    var t = new TextBlock { Text = "-", FontFamily = Mono, FontSize = 13, TextAlignment = TextAlignment.Right, Foreground = new SolidColorBrush(ChannelColors[c]) };
                    Grid.SetRow(t, r + 1);
                    Grid.SetColumn(t, c + 1);
                    grid.Children.Add(t);
                    cells[r, c] = t;
                }
                var sw = new Border { Width = 22, Height = 14, BorderThickness = new Thickness(1), VerticalAlignment = VerticalAlignment.Center,
                                      BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 90, 90, 90)), Visibility = r is 0 or 3 ? Visibility.Visible : Visibility.Collapsed };
                Grid.SetRow(sw, r + 1);
                Grid.SetColumn(sw, 5);
                grid.Children.Add(sw);
                swatches[r] = sw;
            }
            block.Children.Add(grid);
            _analyzerCells[s] = cells;
            _analyzerSwatches[s] = swatches;
            _analyzerBlocks[s] = block;
            AnalyzerHost.Children.Add(block);
        }
    }

    private void UpdateAnalyzerCurrent(JsonObject r)
    {
        if (_analyzerCells[0] is null) BuildAnalyzerTable();
        for (int s = 0; s < 2; s++)
        {
            var px = r[s == 0 ? "a" : "b"]?["pixel"];
            var rgba = px?["rgba"] as JsonArray;
            for (int c = 0; c < 4; c++) _analyzerCells[s][0, c].Text = Fmt(rgba?[c]);
            _analyzerSwatches[s][0].Background = SwatchBrush(px?["display"] as JsonArray);
        }
    }

    private void UpdateAnalyzerTable(JsonObject r)
    {
        if (_analyzerCells[0] is null) BuildAnalyzerTable();
        UpdateAnalyzerCurrent(r);
        bool any = false;
        for (int s = 0; s < 2; s++)
        {
            var l = r[s == 0 ? "a" : "b"];
            _analyzerBlocks[s].Visibility = l is null ? Visibility.Collapsed : Visibility.Visible;
            any |= l is not null;
            var reg = l?["region"];
            string name = s == 0 ? "A" : "B";
            _analyzerTitles[s].Text = reg is null
                ? $"{name}   no sample (Ctrl+drag a box)"
                : $"{name}   x={J.Int(reg, "x")} y={J.Int(reg, "y")} {J.Int(reg, "w")}x{J.Int(reg, "h")}   {J.Int(reg, "count")} px" +
                  (J.Int(reg, "nan") + J.Int(reg, "inf") > 0 ? $"   {J.Int(reg, "nan")} NaN  {J.Int(reg, "inf")} Inf" : "");
            for (int row = 1; row < AnalyzerRows.Length; row++)
            {
                var v = reg?[AnalyzerKeys[row]] as JsonArray;
                for (int c = 0; c < 4; c++) _analyzerCells[s][row, c].Text = Fmt(v?[c]);
            }
            _analyzerSwatches[s][3].Background = SwatchBrush(reg?["display"] as JsonArray);
        }
        AnalyzerEmpty.Visibility = any ? Visibility.Collapsed : Visibility.Visible;
    }

    /// <summary>Cryptomatte / ID names under the pointer, shown below the analyzer table.</summary>
    private void UpdateCryptoDetail(double x, double y)
    {
        var probe = _core.Call(new { cmd = "probe", x, y });
        var detail = new List<string>();
        foreach (var (key, name) in new[] { ("a", "A"), ("b", "B") })
        {
            if (probe[key]?["crypto"] is not JsonArray ranks) continue;
            foreach (var r in ranks.Where(r => J.Num(r, "coverage") > 0))
            {
                string n = J.Str(r, "name");
                detail.Add($"{name}  {(n.Length > 0 ? n : "id " + J.Str(r, "id"))}  coverage {J.Num(r, "coverage"):0.000}");
            }
        }
        ProbeDetail.Text = string.Join("\n", detail);
        ProbeDetail.Visibility = detail.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }
}
