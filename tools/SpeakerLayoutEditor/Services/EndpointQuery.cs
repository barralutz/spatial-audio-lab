using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using SpatialAudioLab.Core.Audio;
using SpatialAudioLab.Core.Runtime;

namespace SpeakerLayoutEditor.Services;

internal sealed class EndpointQuery(RuntimePaths paths)
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    public async Task<IReadOnlyList<AudioEndpointDescriptor>> QueryAsync()
    {
        if (!File.Exists(paths.EngineExecutable))
        {
            throw new FileNotFoundException(
                "No se encontro SpatialAudioLab Engine.",
                paths.EngineExecutable);
        }

        ProcessStartInfo start = new(paths.EngineExecutable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        start.ArgumentList.Add("list-endpoints-json");
        using Process process = Process.Start(start) ??
            throw new InvalidOperationException("No se pudo consultar los endpoints.");
        Task<string> outputTask = process.StandardOutput.ReadToEndAsync();
        Task<string> errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        string output = await outputTask;
        string error = await errorTask;
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                string.IsNullOrWhiteSpace(error)
                    ? "Fallo la consulta de endpoints."
                    : error.Trim());
        }

        List<AudioEndpointDescriptor> endpoints = [];
        foreach (string line in output.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            AudioEndpointDescriptor? endpoint = JsonSerializer.Deserialize<AudioEndpointDescriptor>(
                line.TrimEnd('\r'),
                JsonOptions);
            if (endpoint is null || string.IsNullOrWhiteSpace(endpoint.Id))
            {
                throw new InvalidDataException("Engine devolvio un endpoint JSON invalido.");
            }

            endpoints.Add(endpoint);
        }

        return endpoints;
    }
}
