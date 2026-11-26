using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
using ExrCompare.Native;
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.System;

namespace ExrCompare;

/// <summary>Annotation stroke in layer A image pixels.</summary>
public sealed class Stroke
{
    public List<double[]> Points { get; set; } = new();
    public double[] Color { get; set; } = { 1.0, 0.25, 0.2 };
    public double Width { get; set; } = 3;
}

public sealed partial class MainWindow
{
    // Geometry from the engine, in device pixels.
    private double[]? _rectA, _rectB, _dataA, _dataB;
    private int[]? _imageA;
    private JsonArray? _tilesA, _tilesB;
    private bool _geometryDirty;

    private enum Drag { None, Pan, WipeCenter, WipeRotate, WipeMix, Region, Pen, Click }
    private Drag _drag;
    private Point _dragStart, _dragLast;
    private Point? _lastPointer;
    private Stroke? _currentStroke;
    private readonly Dictionary<int, List<Stroke>> _annotations = new();

    private const double HandleRadius = 110;  // rotate handle distance from the wipe centre (DIPs)
    private const double ArcRadius = 62;      // mix arc radius
    private const double ArcSpan = Math.PI / 2;

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
        _imageA = g["imageA"] is JsonArray ia ? new[] { ia[0]!.GetValue<int>(), ia[1]!.GetValue<int>() } : null;
        _tilesA = g["tilesA"] as JsonArray;
        _tilesB = g["tilesB"] as JsonArray;
        // The engine learns the viewport size on its next frame; re-check soon if it differs.
        var vp = g["viewport"] as JsonArray;
        _geometryDirty = vp is null || vp[0]!.GetValue<int>() != PixelWidth() || vp[1]!.GetValue<int>() != PixelHeight();
        DrawOverlay();
    }

    private Point ToDip(double x, double y) => new(x / Scale, y / Scale);
    private (double x, double y) ToPx(Point p) => (p.X * Scale, p.Y * Scale);

    private (double x, double y) ImageAToDip(double ix, double iy)
    {
        if (_rectA is null || _imageA is null) return (double.NaN, double.NaN);
        double x = _rectA[0] + ix / _imageA[0] * (_rectA[2] - _rectA[0]);
        double y = _rectA[1] + iy / _imageA[1] * (_rectA[3] - _rectA[1]);
        return (x / Scale, y / Scale);
    }

    private double[]? DipToImageA(Point p)
    {
        if (_rectA is null || _imageA is null) return null;
        var (x, y) = ToPx(p);
        return new[] { (x - _rectA[0]) / (_rectA[2] - _rectA[0]) * _imageA[0], (y - _rectA[1]) / (_rectA[3] - _rectA[1]) * _imageA[1] };
    }

    // -------------------------------------------------------------- overlay --
    private (Point c, double angle, double mix) WipeGeometry()
    {
        var w = _status["wipe"];
        double vpW = ViewerArea.ActualWidth, vpH = ViewerArea.ActualHeight;
        return (new Point(J.Num(w, "x", 0.5) * vpW, J.Num(w, "y", 0.5) * vpH), J.Num(w, "angle"), J.Num(w, "mix"));
    }

    private void DrawOverlay()
    {
        if (!_ready) return;
        Overlay.Children.Clear();
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

        // Nuke-style wipe widget: line, centre crosshair, rotate handle, mix arc.
        if (J.Str(_status, "compare") == "wipe" && _rectA is not null && _rectB is not null)
        {
            var (c, a, mix) = WipeGeometry();
            var n = new Point(Math.Cos(a), Math.Sin(a));
            var d = new Point(-n.Y, n.X);
            double L = 8000;
            AddLine(c.X - d.X * L, c.Y - d.Y * L, c.X + d.X * L, c.Y + d.Y * L, shadow, 3);
            AddLine(c.X - d.X * L, c.Y - d.Y * L, c.X + d.X * L, c.Y + d.Y * L, white, 1);
            var h = new Point(c.X + n.X * HandleRadius, c.Y + n.Y * HandleRadius);
            AddLine(c.X, c.Y, h.X, h.Y, white, 1.5);
            AddEllipse(h, 6, white, accent);
            // Mix arc on B's side.
            var arc = new PathFigure { StartPoint = Polar(c, ArcRadius, a - ArcSpan / 2) };
            arc.Segments.Add(new ArcSegment
            {
                Point = Polar(c, ArcRadius, a + ArcSpan / 2), Size = new Size(ArcRadius, ArcRadius),
                SweepDirection = SweepDirection.Clockwise, IsLargeArc = false,
            });
            var geo = new PathGeometry();
            geo.Figures.Add(arc);
            Overlay.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = geo, Stroke = white, StrokeThickness = 1.5 });
            AddEllipse(Polar(c, ArcRadius, a - ArcSpan / 2 + mix * ArcSpan), 5, white, caution);
            AddEllipse(c, 8, white, null);
            AddLine(c.X - 14, c.Y, c.X + 14, c.Y, white, 1);
            AddLine(c.X, c.Y - 14, c.X, c.Y + 14, white, 1);
            AddLabel("A", Polar(c, 26, a + Math.PI), accent);
            AddLabel("B", Polar(c, 26, a), caution);
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
                Overlay.Children.Add(label);
                AddRect(r, new SolidColorBrush(Windows.UI.Color.FromArgb(90, 255, 255, 255)), dashed: false);
            }
        }

        // Annotations of the current frame.
        if (_annotations.TryGetValue(J.Int(_status, "frame"), out var strokes))
            foreach (var s in strokes) AddStroke(s);
        if (_currentStroke is not null) AddStroke(_currentStroke);

        // Region sampler box while dragging.
        if (_drag == Drag.Region)
        {
            var box = new Rectangle
            {
                Width = Math.Abs(_dragLast.X - _dragStart.X), Height = Math.Abs(_dragLast.Y - _dragStart.Y),
                Stroke = white, StrokeThickness = 1, StrokeDashArray = new DoubleCollection { 3, 2 },
            };
            Canvas.SetLeft(box, Math.Min(_dragStart.X, _dragLast.X));
            Canvas.SetTop(box, Math.Min(_dragStart.Y, _dragLast.Y));
            Overlay.Children.Add(box);
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
        Overlay.Children.Add(rect);
    }

    private void AddLine(double x0, double y0, double x1, double y1, Brush brush, double thickness) =>
        Overlay.Children.Add(new Line { X1 = x0, Y1 = y0, X2 = x1, Y2 = y1, Stroke = brush, StrokeThickness = thickness });

    private void AddEllipse(Point c, double r, Brush stroke, Brush? fill)
    {
        var e = new Ellipse { Width = r * 2, Height = r * 2, Stroke = stroke, StrokeThickness = 1.5, Fill = fill };
        Canvas.SetLeft(e, c.X - r);
        Canvas.SetTop(e, c.Y - r);
        Overlay.Children.Add(e);
    }

    private void AddLabel(string text, Point p, Brush brush)
    {
        var t = new TextBlock { Text = text, Foreground = brush, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        Canvas.SetLeft(t, p.X - 4);
        Canvas.SetTop(t, p.Y - 10);
        Overlay.Children.Add(t);
    }

    private void AddStroke(Stroke s)
    {
        var line = new Polyline
        {
            Stroke = new SolidColorBrush(Windows.UI.Color.FromArgb(255, (byte)(s.Color[0] * 255), (byte)(s.Color[1] * 255), (byte)(s.Color[2] * 255))),
            StrokeThickness = s.Width, StrokeLineJoin = PenLineJoin.Round,
            StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
        };
        foreach (var p in s.Points)
        {
            var (x, y) = ImageAToDip(p[0], p[1]);
            if (!double.IsNaN(x)) line.Points.Add(new Point(x, y));
        }
        Overlay.Children.Add(line);
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
        else if (left && PenToggle.IsChecked == true && _rectA is not null)
        {
            _drag = Drag.Pen;
            _currentStroke = new Stroke();
            if (DipToImageA(p) is { } ip) _currentStroke.Points.Add(ip);
        }
        else if (left && J.Str(_status, "compare") == "wipe" && OverlaysToggle.IsChecked == true && HitWipe(p) is { } hit) _drag = hit;
        else if (left) _drag = Drag.Click;  // becomes a pan once it moves
        if (_drag != Drag.None) ViewerArea.CapturePointer(e.Pointer);
    }

    private Drag? HitWipe(Point p)
    {
        var (c, a, mix) = WipeGeometry();
        double Dist(Point q) => Math.Sqrt((p.X - q.X) * (p.X - q.X) + (p.Y - q.Y) * (p.Y - q.Y));
        if (Dist(Polar(c, HandleRadius, a)) < 12) return Drag.WipeRotate;
        if (Dist(Polar(c, ArcRadius, a - ArcSpan / 2 + mix * ArcSpan)) < 12) return Drag.WipeMix;
        if (Dist(c) < 14) return Drag.WipeCenter;
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
                _core.Call(new { cmd = "wipe", x = p.X / ViewerArea.ActualWidth, y = p.Y / ViewerArea.ActualHeight });
                _status = _core.Call(new { cmd = "status" });
                DrawOverlay();
                break;
            case Drag.WipeRotate:
            {
                var (c, _, _) = WipeGeometry();
                _core.Call(new { cmd = "wipe", angle = Math.Atan2(p.Y - c.Y, p.X - c.X) });
                _status = _core.Call(new { cmd = "status" });
                DrawOverlay();
                break;
            }
            case Drag.WipeMix:
            {
                var (c, a, _) = WipeGeometry();
                double rel = Math.IEEERemainder(Math.Atan2(p.Y - c.Y, p.X - c.X) - a, 2 * Math.PI);
                _core.Call(new { cmd = "wipe", mix = Math.Clamp((rel + ArcSpan / 2) / ArcSpan, 0, 1) });
                _status = _core.Call(new { cmd = "status" });
                DrawOverlay();
                break;
            }
            case Drag.Region:
                DrawOverlay();
                break;
            case Drag.Pen:
                if (_currentStroke is not null && DipToImageA(p) is { } ip)
                {
                    _currentStroke.Points.Add(ip);
                    DrawOverlay();
                }
                break;
        }
        _dragLast = p;
        UpdateProbe(p);
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
                var (x0, y0) = ToPx(_dragStart);
                var (x1, y1) = ToPx(p);
                ShowRegion(_core.Call(new { cmd = "region", x0, y0, x1, y1 }));
                break;
            }
            case Drag.Pen:
                if (_currentStroke is { Points.Count: > 0 })
                {
                    int frame = J.Int(_status, "frame");
                    if (!_annotations.TryGetValue(frame, out var list)) _annotations[frame] = list = new List<Stroke>();
                    list.Add(_currentStroke);
                }
                _currentStroke = null;
                break;
        }
        _drag = Drag.None;
        ViewerArea.ReleasePointerCapture(e.Pointer);
        DrawOverlay();
    }

    private void OnViewerPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (_drag != Drag.None) return;
        _lastPointer = null;
        ProbeText.Text = "";
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

    private void OnViewerDoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => ViewAction(new { cmd = "view", action = "fit" });

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

    private void UpdateProbe(Point p)
    {
        if (!J.Bool(_status, "hasContent")) return;
        var (x, y) = ToPx(p);
        var probe = _core.Call(new { cmd = "probe", x, y });
        var parts = new List<string>();
        var detail = new List<string>();
        foreach (var (key, name) in new[] { ("a", "A"), ("b", "B") })
        {
            var v = probe[key];
            if (v is null) continue;
            var c = v["rgba"] as JsonArray;
            string values = c is null ? "" : $"R {J.Value(c[0])}  G {J.Value(c[1])}  B {J.Value(c[2])}  A {J.Value(c[3])}";
            parts.Add($"{name} {J.Int(v, "x"),5},{J.Int(v, "y"),-5} {values}");
            detail.Add($"{name}  x {J.Int(v, "x")}  y {J.Int(v, "y")}\n   {values}");
            if (v["crypto"] is JsonArray ranks)
                foreach (var r in ranks.Where(r => J.Num(r, "coverage") > 0))
                {
                    string n = J.Str(r, "name");
                    detail.Add($"   {(n.Length > 0 ? n : "id " + J.Str(r, "id"))}  coverage {J.Num(r, "coverage"):0.000}");
                    if (parts.Count > 0) parts[^1] += $"  {(n.Length > 0 ? n : J.Str(r, "id"))}";
                }
        }
        ProbeText.Text = string.Join("    ", parts);
        if (InfoPage.Visibility == Visibility.Visible) ProbeDetail.Text = detail.Count > 0 ? string.Join("\n", detail) : "Outside the image.";
    }

    private void ShowRegion(JsonObject r)
    {
        var lines = new List<string>();
        foreach (var (key, name) in new[] { ("a", "A"), ("b", "B") })
        {
            var v = r[key];
            if (v is null) continue;
            string Arr(string k) => string.Join("  ", ((v[k] as JsonArray) ?? new JsonArray()).Select(x => J.Value(x)));
            lines.Add($"{name}  {J.Int(v, "count")} px{(J.Int(v, "nan") + J.Int(v, "inf") > 0 ? $"  ({J.Int(v, "nan")} NaN, {J.Int(v, "inf")} Inf)" : "")}");
            lines.Add($"   mean {Arr("mean")}");
            lines.Add($"   min  {Arr("min")}");
            lines.Add($"   max  {Arr("max")}");
        }
        RegionDetail.Text = lines.Count > 0 ? string.Join("\n", lines) : "No image under the box.";
        PaneBar.SelectedItem = PaneBar.Items[1];
    }

    private void OnPenToggle(object sender, RoutedEventArgs e) => PenMenuItem.IsChecked = PenToggle.IsChecked == true;
    private void OnPenMenu(object sender, RoutedEventArgs e) => PenToggle.IsChecked = PenMenuItem.IsChecked;

    private void OnClearAnnotations(object sender, RoutedEventArgs e)
    {
        _annotations.Remove(J.Int(_status, "frame"));
        DrawOverlay();
    }
}
