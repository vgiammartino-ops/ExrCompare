using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace ExrCompare.Controls;

/// <summary>
/// ComboBox whose closed box steps one item per mouse-wheel notch (RV / Nuke style). The stock
/// ComboBox also reacts to the wheel, so handling it in a separate event handler stepped twice in
/// one direction; overriding the class handler gives exactly one step per notch both ways.
/// </summary>
public class WheelComboBox : ComboBox
{
    private int _wheel;

    public WheelComboBox()
    {
        DefaultStyleKey = typeof(ComboBox);
    }

    protected override void OnPointerWheelChanged(PointerRoutedEventArgs e)
    {
        if (IsDropDownOpen)
        {
            base.OnPointerWheelChanged(e);  // the open list scrolls normally
            return;
        }
        e.Handled = true;
        if (!IsEnabled || Items.Count == 0) return;
        _wheel += e.GetCurrentPoint(this).Properties.MouseWheelDelta;
        int step = 0;
        while (_wheel >= 120) { step--; _wheel -= 120; }
        while (_wheel <= -120) { step++; _wheel += 120; }
        if (step == 0) return;
        int next = System.Math.Clamp(SelectedIndex + step, 0, Items.Count - 1);
        if (next != SelectedIndex) SelectedIndex = next;
    }
}
