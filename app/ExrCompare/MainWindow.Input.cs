using ExrCompare.Native;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace ExrCompare;

/// <summary>Keyboard shortcuts (RV / Nuke conventions where they exist).</summary>
public sealed partial class MainWindow
{
    private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        // Text and number fields keep their keys.
        var focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        if (focused is TextBox || focused is NumberBox || focused is AutoSuggestBox) return;
        bool ctrl = IsKeyDown(VirtualKey.Control), shift = IsKeyDown(VirtualKey.Shift), alt = IsKeyDown(VirtualKey.Menu);
        bool handled = true;
        switch (e.Key)
        {
            case VirtualKey.Space:
                if (ctrl) OnPlayReverse(this, new RoutedEventArgs());
                else Send(new { cmd = "toggle" });
                break;
            case VirtualKey.Right: Send(new { cmd = "step", delta = 1 }); break;
            case VirtualKey.Left: Send(new { cmd = "step", delta = -1 }); break;
            case VirtualKey.Home: Send(new { cmd = "start" }); break;
            case VirtualKey.End: Send(new { cmd = "end" }); break;
            case VirtualKey.PageUp: LayerPanelA.StepAov(-1); if (!J.Bool(_status, "lockAov", true)) LayerPanelB.StepAov(-1); break;
            case VirtualKey.PageDown: LayerPanelA.StepAov(+1); if (!J.Bool(_status, "lockAov", true)) LayerPanelB.StepAov(+1); break;
            case VirtualKey.Up when alt: ChangeVersion(shift ? 1 : 0, +1); break;
            case VirtualKey.Down when alt: ChangeVersion(shift ? 1 : 0, -1); break;
            case VirtualKey.W: ToggleWipe(); break;
            case VirtualKey.D: SetCompare("difference"); break;
            case VirtualKey.S when ctrl: _ = SaveSession(); break;
            case VirtualKey.S: SetCompare("stack"); break;
            case VirtualKey.Enter: OnSwap(this, new RoutedEventArgs()); break;
            case VirtualKey.F: OnFit(this, new RoutedEventArgs()); break;
            case VirtualKey.Number1:
            case VirtualKey.NumberPad1: OnOneToOne(this, new RoutedEventArgs()); break;
            case VirtualKey.Number0:
            case VirtualKey.NumberPad0: ResetExposure(); break;
            case VirtualKey.C: SetChannel(0); break;
            case VirtualKey.R: SetChannel(1); break;
            case VirtualKey.G: SetChannel(2); break;
            case VirtualKey.B: SetChannel(3); break;
            case VirtualKey.A: SetChannel(4); break;
            case VirtualKey.Y: SetChannel(5); break;
            case VirtualKey.M: ToggleMatte(); break;
            case VirtualKey.Q: SetOverlays(OverlaysToggle.IsChecked != true); break;
            case VirtualKey.T: LayerPanelA.ToggleContactSheet(); break;
            case VirtualKey.P:
                PenToggle.IsChecked = PenToggle.IsChecked != true;
                PenMenuItem.IsChecked = PenToggle.IsChecked == true;
                break;
            case VirtualKey.I:
                if (shift) Send(new { cmd = "clear_inout" });
                else OnSetIn(this, new RoutedEventArgs());
                break;
            case VirtualKey.O when ctrl: _ = PickAndOpen(shift ? 1 : 0, folder: false); break;
            case VirtualKey.O: OnSetOut(this, new RoutedEventArgs()); break;
            case VirtualKey.E when ctrl: _ = Snapshot(); break;
            case VirtualKey.F11: ToggleFullScreen(); break;
            case VirtualKey.Escape:
                if (AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen) ToggleFullScreen();
                else handled = false;
                break;
            default: handled = false; break;
        }
        e.Handled = handled;
    }
}
