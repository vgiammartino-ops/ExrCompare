using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json.Nodes;
using ExrCompare.Native;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;

namespace ExrCompare;

/// <summary>Info (probe, region, stats, notes, metadata) and Scopes (histogram, waveform) panes.</summary>
public sealed partial class MainWindow
{
    private readonly Dictionary<int, string> _notes = new();
    private List<string> _metadataRows = new();
    private bool _loadingNote;
    private WriteableBitmap? _waveformBitmap;
    private readonly byte[] _waveformPixels = new byte[256 * 128 * 4];

    private void RefreshSidePanels(bool force)
    {
        if (!_ready) return;
        if (InfoPage.Visibility == Visibility.Visible)
        {
            if (force) RefreshMetadata();
            RefreshStats();
        }
        if (ScopesPage.Visibility == Visibility.Visible) RefreshScopes();
    }

    // ------------------------------------------------------------------ stats --
    private void OnRefreshStats(object sender, RoutedEventArgs e) => RefreshStats();

    private void RefreshStats()
    {
        if (!_ready) return;
        var r = _core.Call(new { cmd = "stats" });
        var lines = new List<string>();
        string[] ch = { "R", "G", "B", "A" };
        foreach (var (key, name) in new[] { ("a", "A"), ("b", "B") })
        {
            if (r[key] is not JsonArray chans) continue;
            lines.Add($"{name}            min        max       mean   NaN/Inf  neg");
            for (int c = 0; c < chans.Count && c < 4; c++)
            {
                var s = chans[c];
                lines.Add($"  {ch[c]}  {J.Num(s, "min"),11:0.0000} {J.Num(s, "max"),10:0.0000} {J.Num(s, "mean"),10:0.0000}" +
                          $"  {J.Int(s, "nan") + J.Int(s, "inf"),7}  {J.Int(s, "negative"),4}");
            }
        }
        StatsDetail.Text = lines.Count > 0 ? string.Join("\n", lines) : "No decoded frame yet.";
    }

    // --------------------------------------------------------------- metadata --
    private void OnMetaSlotChanged(object sender, SelectionChangedEventArgs e) => RefreshMetadata();
    private void OnMetaFilterChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args) => ApplyMetadataFilter();

    private void RefreshMetadata()
    {
        if (!_ready) return;
        int slot = Math.Max(0, MetaSlotButtons.SelectedIndex);
        var r = _core.Call(new { cmd = "metadata", slot });
        _metadataRows = ((r["metadata"] as JsonArray) ?? new JsonArray())
            .Select(m => $"{(J.Int(m, "part") > 0 ? $"[{J.Int(m, "part")}] " : "")}{J.Str(m, "name")}  ({J.Str(m, "type")})\n    {J.Str(m, "value")}")
            .ToList();
        ApplyMetadataFilter();
    }

    private void ApplyMetadataFilter()
    {
        if (!_ready) return;
        string f = MetaFilter.Text ?? "";
        MetadataList.Items.Clear();
        foreach (var row in _metadataRows.Where(r => f.Length == 0 || r.Contains(f, StringComparison.OrdinalIgnoreCase)))
            MetadataList.Items.Add(new TextBlock { Text = row, TextWrapping = TextWrapping.Wrap, FontSize = 12, IsTextSelectionEnabled = true });
    }

    // ------------------------------------------------------------------ notes --
    private void LoadNote()
    {
        if (!_ready) return;
        _loadingNote = true;
        NoteBox.Text = _notes.TryGetValue(J.Int(_status, "frame"), out var n) ? n : "";
        _loadingNote = false;
    }

    private void OnNoteChanged(object sender, TextChangedEventArgs e)
    {
        if (_loadingNote || !_ready) return;
        int frame = J.Int(_status, "frame");
        if (string.IsNullOrWhiteSpace(NoteBox.Text)) _notes.Remove(frame);
        else _notes[frame] = NoteBox.Text;
    }

    // ----------------------------------------------------------------- scopes --
    private void OnScopeSlotChanged(object sender, SelectionChangedEventArgs e) => RefreshScopes();
    private void OnScopeSizeChanged(object sender, SizeChangedEventArgs e) => RefreshScopes();

    private void RefreshScopes()
    {
        if (!_ready) return;
        int slot = Math.Max(0, ScopeSlotButtons.SelectedIndex);
        var r = _core.Call(new { cmd = "histogram", slot, bins = 128 });
        HistogramCanvas.Children.Clear();
        if (r["histogram"] is JsonObject h)
        {
            double w = HistogramCanvas.ActualWidth, ht = HistogramCanvas.ActualHeight;
            var series = new[] { ("luma", Windows.UI.Color.FromArgb(150, 200, 200, 200)), ("r", Windows.UI.Color.FromArgb(220, 255, 70, 70)),
                                 ("g", Windows.UI.Color.FromArgb(220, 70, 220, 70)), ("b", Windows.UI.Color.FromArgb(220, 80, 130, 255)) };
            double max = 1;
            foreach (var (key, _) in series)
                foreach (var v in (h[key] as JsonArray) ?? new JsonArray()) max = Math.Max(max, Math.Sqrt(v!.GetValue<double>()));
            foreach (var (key, color) in series)
            {
                var values = (h[key] as JsonArray) ?? new JsonArray();
                var line = new Polyline { Stroke = new SolidColorBrush(color), StrokeThickness = 1.2 };
                for (int i = 0; i < values.Count; i++)
                    line.Points.Add(new Point(i / (double)Math.Max(1, values.Count - 1) * w, ht - Math.Sqrt(values[i]!.GetValue<double>()) / max * (ht - 4)));
                HistogramCanvas.Children.Add(line);
            }
        }

        if (_core.Waveform(slot, 256, 128, _waveformPixels))
        {
            _waveformBitmap ??= new WriteableBitmap(256, 128);
            var bgra = new byte[_waveformPixels.Length];
            for (int i = 0; i < bgra.Length; i += 4)
            {
                bgra[i] = _waveformPixels[i + 2];
                bgra[i + 1] = _waveformPixels[i + 1];
                bgra[i + 2] = _waveformPixels[i];
                bgra[i + 3] = 255;
            }
            using (Stream s = _waveformBitmap.PixelBuffer.AsStream())
            {
                s.Write(bgra, 0, bgra.Length);
            }
            _waveformBitmap.Invalidate();
            WaveformImage.Source = _waveformBitmap;
        }
        else
        {
            WaveformImage.Source = null;
        }
    }
}
