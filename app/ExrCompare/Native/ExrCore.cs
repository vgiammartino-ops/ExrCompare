using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace ExrCompare.Native;

/// <summary>
/// Thin wrapper over exrcore.dll. Everything except rendering setup is a JSON command:
/// <c>Call(new { cmd = "open", slot = 0, path = "..." })</c> returns the engine's JSON reply.
/// </summary>
public sealed class ExrCore : IDisposable
{
    private IntPtr _handle;
    private byte[] _buffer = new byte[256 * 1024];

    public ExrCore(long cacheBudgetBytes)
    {
        string config = JsonSerializer.Serialize(new { cacheBudget = cacheBudgetBytes });
        _handle = exrc_create(Utf8(config));
        if (_handle == IntPtr.Zero) throw new InvalidOperationException("exrcore failed to start");
    }

    /// <summary>Runs a command. Returns the reply object (with "ok" and, on failure, "error").</summary>
    public JsonObject Call(object command)
    {
        string json = command is string s ? s : JsonSerializer.Serialize(command);
        byte[] input = Utf8(json);
        int length = exrc_call(_handle, input, _buffer, _buffer.Length);
        if (length >= _buffer.Length)
        {
            _buffer = new byte[length + 1];
            length = exrc_call(_handle, input, _buffer, _buffer.Length);
        }
        string text = Encoding.UTF8.GetString(_buffer, 0, Math.Max(0, Math.Min(length, _buffer.Length - 1)));
        return JsonNode.Parse(text) as JsonObject ?? new JsonObject { ["ok"] = false, ["error"] = "bad reply" };
    }

    public static bool Ok(JsonObject reply) => reply["ok"]?.GetValue<bool>() == true;
    public static string Error(JsonObject reply) => reply["error"]?.GetValue<string>() ?? "Unknown error";

    public int AttachPanel(IntPtr panelUnknown, int width, int height, float scaleX, float scaleY) =>
        exrc_attach_panel(_handle, panelUnknown, width, height, scaleX, scaleY);

    public void Resize(int width, int height, float scaleX, float scaleY) =>
        exrc_resize(_handle, width, height, scaleX, scaleY);

    public bool Waveform(int slot, int width, int height, byte[] rgba) =>
        exrc_waveform(_handle, slot, width, height, rgba) == 0;

    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
        {
            exrc_detach(_handle);
            exrc_destroy(_handle);
            _handle = IntPtr.Zero;
        }
    }

    private static byte[] Utf8(string s)
    {
        byte[] bytes = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, 0, s.Length, bytes, 0);
        return bytes;  // NUL-terminated
    }

    private const string Dll = "exrcore";

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr exrc_create(byte[] configJson);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void exrc_destroy(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int exrc_call(IntPtr handle, byte[] json, [Out] byte[] output, int capacity);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int exrc_attach_panel(IntPtr handle, IntPtr panel, int width, int height, float scaleX, float scaleY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void exrc_resize(IntPtr handle, int width, int height, float scaleX, float scaleY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void exrc_detach(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int exrc_waveform(IntPtr handle, int slot, int width, int height, [Out] byte[] rgba);
}

/// <summary>Small helpers for reading engine JSON replies.</summary>
public static class J
{
    public static int Int(JsonNode? n, string key, int fallback = 0) =>
        n?[key] is JsonValue v && v.TryGetValue(out double d) ? (int)d : fallback;

    public static double Num(JsonNode? n, string key, double fallback = 0) =>
        n?[key] is JsonValue v && v.TryGetValue(out double d) ? d : fallback;

    public static bool Bool(JsonNode? n, string key, bool fallback = false) =>
        n?[key] is JsonValue v && v.TryGetValue(out bool b) ? b : fallback;

    public static string Str(JsonNode? n, string key, string fallback = "") =>
        n?[key] is JsonValue v && v.TryGetValue(out string? s) && s is not null ? s : fallback;

    public static double[]? Rect(JsonNode? n, string key)
    {
        if (n?[key] is not JsonArray a || a.Count != 4) return null;
        var r = new double[4];
        for (int i = 0; i < 4; i++) r[i] = a[i]!.GetValue<double>();
        return r;
    }

    /// <summary>Formats a probe value that may be a number or "NaN"/"+Inf"/"-Inf".</summary>
    public static string Value(JsonNode? v) =>
        v is JsonValue jv && jv.TryGetValue(out double d) ? d.ToString("0.0000") : v?.ToString() ?? "-";
}
