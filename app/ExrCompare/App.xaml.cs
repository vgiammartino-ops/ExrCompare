using System;
using System.IO;
using Microsoft.UI.Xaml;

namespace ExrCompare;

public partial class App : Application
{
    private Window? _window;

    /// <summary>%LOCALAPPDATA%\ExrCompare\startup-error.txt: written if the app fails to start.</summary>
    public static string ErrorLogPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ExrCompare", "startup-error.txt");

    public static void LogError(string where, Exception? ex)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ErrorLogPath)!);
            File.AppendAllText(ErrorLogPath, $"{DateTime.Now:o} {where}\n{ex}\n\n");
        }
        catch
        {
            // Logging must never take the app down.
        }
    }

    public App()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) => LogError("AppDomain", e.ExceptionObject as Exception);
        try
        {
            InitializeComponent();
        }
        catch (Exception ex)
        {
            LogError("App.InitializeComponent", ex);
            throw;
        }
        UnhandledException += (_, e) =>
        {
            LogError("XAML", e.Exception);
            // Keep the viewer alive on UI errors; they are reported in the info bar.
            e.Handled = true;
            (_window as MainWindow)?.ReportError(e.Exception?.Message ?? e.Message);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            _window = new MainWindow();
            _window.Activate();
        }
        catch (Exception ex)
        {
            LogError("MainWindow", ex);
            throw;
        }
    }
}
