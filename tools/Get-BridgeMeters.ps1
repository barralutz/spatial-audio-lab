[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Core

$mappingName = 'Local\DolbyDecoderBridgeMetersV1'
try {
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
        $mappingName,
        [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
} catch {
    throw 'No SpatialAudioLab bridge meter is currently available.'
}

try {
    $view = $mapping.CreateViewAccessor(
        0, 1320, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
    try {
        $snapshot = $null
        for ($attempt = 0; $attempt -lt 20 -and $null -eq $snapshot; ++$attempt) {
            $sequenceBefore = $view.ReadInt32(8)
            if (($sequenceBefore -band 1) -ne 0) {
                Start-Sleep -Milliseconds 1
                continue
            }

            $magic = $view.ReadUInt32(0)
            $version = $view.ReadUInt32(4)
            $channelCount = $view.ReadUInt32(12)
            $mode = $view.ReadUInt32(16)
            $updateCounter = $view.ReadUInt64(24)
            $performanceCounter = $view.ReadInt64(32)
            if ($magic -ne 0x544D4244 -or $version -ne 1 -or $channelCount -gt 32) {
                throw 'The bridge meter mapping has an incompatible format.'
            }

            $channels = @()
            for ($channel = 0; $channel -lt $channelCount; ++$channel) {
                $nameBytes = New-Object byte[] 32
                $null = $view.ReadArray(296 + $channel * 32, $nameBytes, 0, 32)
                $name = [Text.Encoding]::Unicode.GetString($nameBytes).TrimEnd([char]0)
                $channels += [pscustomobject]@{
                    Channel = $name
                    Rms = $view.ReadSingle(40 + $channel * 4)
                    Peak = $view.ReadSingle(168 + $channel * 4)
                }
            }

            $sequenceAfter = $view.ReadInt32(8)
            if ($sequenceBefore -eq $sequenceAfter -and ($sequenceAfter -band 1) -eq 0) {
                $snapshot = [pscustomobject]@{
                    Mode = $mode
                    UpdateCounter = $updateCounter
                    PerformanceCounter = $performanceCounter
                    Channels = $channels
                }
            }
        }
        if ($null -eq $snapshot) { throw 'Could not read a stable bridge meter snapshot.' }

        $modeName = switch ($snapshot.Mode) {
            1 { 'MAT' }
            2 { 'DTS:X' }
            3 { 'PCM' }
            default { "Unknown($($snapshot.Mode))" }
        }
        $ageMilliseconds = if ($snapshot.PerformanceCounter -eq 0) {
            [double]::PositiveInfinity
        } else {
            1000.0 * ([Diagnostics.Stopwatch]::GetTimestamp() -
                $snapshot.PerformanceCounter) / [Diagnostics.Stopwatch]::Frequency
        }

        Write-Host ("Bridge mode={0}, updates={1}, last update age={2:N1} ms" -f `
            $modeName, $snapshot.UpdateCounter, $ageMilliseconds)
        $snapshot.Channels | ForEach-Object {
            [pscustomobject]@{
                Channel = $_.Channel
                RmsDbfs = if ($_.Rms -gt 0) {
                    ([Math]::Round(20 * [Math]::Log10($_.Rms), 1)).ToString('0.0')
                } else { '-inf' }
                PeakDbfs = if ($_.Peak -gt 0) {
                    ([Math]::Round(20 * [Math]::Log10($_.Peak), 1)).ToString('0.0')
                } else { '-inf' }
            }
        } | Format-Table -AutoSize
    } finally {
        $view.Dispose()
    }
} finally {
    $mapping.Dispose()
}
