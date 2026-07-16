using System.Diagnostics;

namespace DolbyPlayer;

internal static class ProcessRunner {
    public static async Task<string> CaptureAsync(string executable, IEnumerable<string> arguments,
                                                   CancellationToken cancellationToken) {
        ProcessStartInfo start = CreateStartInfo(executable, arguments);
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        using Process process = Process.Start(start) ?? throw new InvalidOperationException(
            $"Failed to start {executable}");
        Task<string> stdout = process.StandardOutput.ReadToEndAsync(cancellationToken);
        Task<string> stderr = process.StandardError.ReadToEndAsync(cancellationToken);
        await WaitAsync(process, cancellationToken);
        string output = await stdout;
        string error = await stderr;
        if (process.ExitCode != 0) {
            throw new InvalidOperationException(
                $"{Path.GetFileName(executable)} exited with {process.ExitCode}: {error.Trim()}");
        }
        return output;
    }

    static ProcessStartInfo CreateStartInfo(string executable, IEnumerable<string> arguments) {
        ProcessStartInfo result = new(executable) {
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (string argument in arguments) result.ArgumentList.Add(argument);
        return result;
    }

    static async Task WaitAsync(Process process, CancellationToken cancellationToken) {
        try {
            await process.WaitForExitAsync(cancellationToken);
        } catch (OperationCanceledException) {
            try { process.Kill(true); } catch { }
            throw;
        }
    }
}
