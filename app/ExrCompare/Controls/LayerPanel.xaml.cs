using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
using ExrCompare.Native;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;

namespace ExrCompare.Controls;

/// <summary>Layer card for slot A (0) or B (1): AOV, display mode, colour space, opacity, offset ...</summary>
public sealed partial class LayerPanel : UserControl
{
    private static readonly string[] ModeKeys = { "auto", "color", "raw", "depth", "normal", "motion", "id" };

    private bool _updating;
    private List<string> _aovNames = new();
    private List<string> _colorSpaces = new();
    private List<string> _channels = new();

    public int Slot { get; private set; }
    /// <summary>Sends a command to the engine (slot is added automatically) and returns the reply.</summary>
    public Func<object, JsonObject>? Send { get; set; }
    public Action? Changed { get; set; }
    public Action<int>? OpenRequested { get; set; }
    public Action<int, int>? VersionRequested { get; set; }
    public Action<int>? CloseRequested { get; set; }
    /// <summary>Raised after every refresh with the "layer" reply (the toolbar AOV menu mirrors it).</summary>
    public Action<int, JsonObject>? Updated { get; set; }

    public LayerPanel()
    {
        _updating = true;  // XAML fires change handlers while it loads; ignore them
        InitializeComponent();
        _updating = false;
    }

    public void SetSlot(int slot)
    {
        Slot = slot;
        BadgeText.Text = slot == 0 ? "A" : "B";
        if (slot == 1)
            Badge.Background = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["SystemFillColorCautionBrush"];
    }

    public bool HasSequence { get; private set; }
    public int AovCount => _aovNames.Count;

    private void SetLayer(object extra)
    {
        if (_updating || Send is null) return;
        var cmd = new JsonObject { ["cmd"] = "layer_set", ["slot"] = Slot };
        foreach (var kv in JsonNode.Parse(System.Text.Json.JsonSerializer.Serialize(extra))!.AsObject())
            cmd[kv.Key] = kv.Value?.DeepClone();
        var reply = Send(cmd.ToJsonString());
        Update(reply);
        Changed?.Invoke();
    }

    /// <summary>Refreshes every control from a "layer" reply.</summary>
    public void Update(JsonObject? layer, IReadOnlyList<string>? colorSpaces = null)
    {
        if (layer is null || !ExrCore.Ok(layer)) return;
        _updating = true;
        try
        {
            if (colorSpaces is not null && !colorSpaces.SequenceEqual(_colorSpaces))
            {
                _colorSpaces = colorSpaces.ToList();
                InputCombo.Items.Clear();
                foreach (var cs in _colorSpaces) InputCombo.Items.Add(cs);
            }
            HasSequence = J.Bool(layer, "loaded");
            Body.Visibility = HasSequence ? Visibility.Visible : Visibility.Collapsed;
            CloseButton.IsEnabled = HasSequence;
            if (!HasSequence)
            {
                NameText.Text = "Empty - open a sequence";
                InfoText.Text = "";
                VersionUpButton.IsEnabled = VersionDownButton.IsEnabled = false;
                _aovNames.Clear();
                return;
            }
            NameText.Text = J.Str(layer, "pattern");
            ToolTipService.SetToolTip(NameText, J.Str(layer, "directory"));
            string version = J.Str(layer, "version");
            VersionUpButton.IsEnabled = VersionDownButton.IsEnabled = version.Length > 0;
            int missing = (layer["missing"] as JsonArray)?.Count ?? 0;
            InfoText.Text = $"{J.Int(layer, "first")}-{J.Int(layer, "last")}   {J.Int(layer, "width")} x {J.Int(layer, "height")}   " +
                            $"{J.Str(layer, "compression")}{(J.Bool(layer, "tiled") ? ", tiled" : "")}" +
                            $"{(version.Length > 0 ? "   " + version : "")}{(missing > 0 ? $"   {missing} missing" : "")}";

            // AOVs
            var aovs = (layer["aovs"] as JsonArray) ?? new JsonArray();
            var names = aovs.Select(a => J.Str(a, "name")).ToList();
            if (!names.SequenceEqual(_aovNames))
            {
                _aovNames = names;
                AovCombo.Items.Clear();
                BeautyCombo.Items.Clear();
                RebuildList.Children.Clear();
                foreach (var a in aovs)
                {
                    string name = J.Str(a, "name");
                    string kind = J.Str(a, "kind");
                    var item = new ComboBoxItem { Content = kind == "color" ? name : $"{name}   ({kind})", Tag = name };
                    ToolTipService.SetToolTip(item, J.Str(a, "channels"));
                    AovCombo.Items.Add(item);
                    BeautyCombo.Items.Add(name);
                    if (!J.Bool(a, "data"))
                    {
                        var box = new CheckBox { Content = name, Tag = name };
                        box.Click += (_, _) => SendRebuildAovs();
                        RebuildList.Children.Add(box);
                    }
                }
            }
            AovCombo.SelectedIndex = J.Int(layer, "aov");

            // Stereo views
            var views = (layer["views"] as JsonArray)?.Select(v => v!.GetValue<string>()).ToList() ?? new();
            ViewCombo.Visibility = views.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
            if (views.Count > 1)
            {
                ViewCombo.Items.Clear();
                foreach (var v in views) ViewCombo.Items.Add(v);
                string view = J.Str(layer, "view");
                ViewCombo.SelectedIndex = Math.Max(0, views.IndexOf(view.Length > 0 ? view : views[0]));
            }

            ModeCombo.SelectedIndex = Math.Max(0, Array.IndexOf(ModeKeys, J.Str(layer, "mode", "auto")));
            InputCombo.SelectedIndex = _colorSpaces.IndexOf(J.Str(layer, "inputSpace"));
            OpacitySlider.Value = J.Num(layer, "opacity", 1);
            VisibleSwitch.IsChecked = J.Bool(layer, "visible", true);
            OffsetBox.Value = J.Int(layer, "offset");

            var channels = (layer["channels"] as JsonArray)?.Select(c => c!.GetValue<string>()).ToList() ?? new();
            if (!channels.SequenceEqual(_channels))
            {
                _channels = channels;
                AlphaCombo.Items.Clear();
                AlphaCombo.Items.Add("(AOV's own alpha)");
                foreach (var c in _channels) AlphaCombo.Items.Add(c);
            }
            string alpha = J.Str(layer, "alpha");
            AlphaCombo.SelectedIndex = alpha.Length == 0 ? 0 : _channels.IndexOf(alpha) + 1;

            string effective = J.Str(layer, "effectiveMode");
            DepthPanel.Visibility = effective == "depth" ? Visibility.Visible : Visibility.Collapsed;
            DepthAutoSwitch.IsChecked = J.Bool(layer, "depthAuto", true);
            DepthNearBox.Value = J.Num(layer, "depthNear");
            DepthFarBox.Value = J.Num(layer, "depthFar", 1);
            DepthNearBox.IsEnabled = DepthFarBox.IsEnabled = DepthAutoSwitch.IsChecked != true;
            DepthInvertBox.IsChecked = J.Bool(layer, "depthInvert");
            MotionScaleBox.Visibility = effective == "motion" ? Visibility.Visible : Visibility.Collapsed;
            MotionScaleBox.Value = J.Num(layer, "motionScale", 0.05);
            if (effective != "id") IsolatePanel.Visibility = Visibility.Collapsed;

            ContactToggle.IsChecked = J.Bool(layer, "contactSheet");
            BeautyCombo.SelectedItem = J.Str(layer, "rebuildBeauty");
            RebuildModeButtons.SelectedIndex = J.Int(layer, "rebuild");
            var ticked = (layer["rebuildAovs"] as JsonArray)?.Select(x => x!.GetValue<string>()).ToHashSet() ?? new();
            foreach (var child in RebuildList.Children.OfType<CheckBox>())
                child.IsChecked = ticked.Contains((string)child.Tag);
        }
        finally
        {
            _updating = false;
        }
        Updated?.Invoke(Slot, layer);
    }

    public void ShowIsolate(string? text)
    {
        IsolatePanel.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
        IsolateText.Text = text ?? "";
    }

    // ------------------------------------------------------------------ AOV --
    private void OnAovChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        if (_updating || Send is null || AovCombo.SelectedIndex < 0) return;
        Update(Send(new { cmd = "aov", slot = Slot, index = AovCombo.SelectedIndex }));
        Changed?.Invoke();
    }

    public void StepAov(int delta)
    {
        if (Send is null || !HasSequence) return;
        Update(Send(new { cmd = "aov", slot = Slot, delta }));
        Changed?.Invoke();
    }

    private void OnViewChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        if (ViewCombo.SelectedItem is string v) SetLayer(new { view = v });
    }

    private void OnModeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        if (ModeCombo.SelectedIndex >= 0) SetLayer(new { mode = ModeKeys[ModeCombo.SelectedIndex] });
    }

    private void OnInputChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        if (InputCombo.SelectedItem is string cs) SetLayer(new { inputSpace = cs });
    }

    private void OnOpacityChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_updating) return;
        SetLayer(new { opacity = e.NewValue });
    }
    private void OnVisibleToggled(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        SetLayer(new { visible = VisibleSwitch.IsChecked == true });
    }

    private void OnOffsetChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    {
        if (_updating) return;
        if (!double.IsNaN(e.NewValue)) SetLayer(new { offset = (int)Math.Round(e.NewValue) });
    }

    private void OnAlphaChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        if (AlphaCombo.SelectedIndex < 0) return;
        SetLayer(new { alpha = AlphaCombo.SelectedIndex == 0 ? "" : (string)AlphaCombo.SelectedItem });
    }

    private void OnDepthChanged(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        SetLayer(new { depthAuto = DepthAutoSwitch.IsChecked == true, depthInvert = DepthInvertBox.IsChecked == true });
    }

    private void OnDepthValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    {
        if (_updating) return;
        if (double.IsNaN(DepthNearBox.Value) || double.IsNaN(DepthFarBox.Value)) return;
        SetLayer(new { depthNear = DepthNearBox.Value, depthFar = DepthFarBox.Value });
    }

    private void OnMotionScaleChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    {
        if (_updating) return;
        if (!double.IsNaN(e.NewValue)) SetLayer(new { motionScale = e.NewValue });
    }

    private void OnClearIsolate(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        SetLayer(new { isolate = (uint?)null });
        ShowIsolate(null);
    }

    private void OnContactToggled(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        SetLayer(new { contactSheet = ContactToggle.IsChecked == true });
    }
    public void ToggleContactSheet() => SetLayer(new { contactSheet = ContactToggle.IsChecked != true });

    private void OnRebuildChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        SetLayer(new { rebuild = Math.Max(0, RebuildModeButtons.SelectedIndex), rebuildBeauty = BeautyCombo.SelectedItem as string ?? "" });
    }

    private void SendRebuildAovs()
    {
        var names = RebuildList.Children.OfType<CheckBox>().Where(c => c.IsChecked == true).Select(c => (string)c.Tag).ToArray();
        SetLayer(new { rebuildAovs = names });
    }

    // --------------------------------------------------------------- header --
    private void OnOpen(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        OpenRequested?.Invoke(Slot);
    }
    private void OnVersionUp(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        VersionRequested?.Invoke(Slot, +1);
    }
    private void OnVersionDown(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        VersionRequested?.Invoke(Slot, -1);
    }
    private void OnClose(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        CloseRequested?.Invoke(Slot);
    }
}
