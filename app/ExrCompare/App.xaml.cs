using Microsoft.UI.Xaml;

namespace ExrCompare;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) =>
        {
            // Keep the viewer alive on UI errors; they are reported in the info bar.
            e.Handled = true;
            (_window as MainWindow)?.ReportError(e.Exception?.Message ?? e.Message);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}
