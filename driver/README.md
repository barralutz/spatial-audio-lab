# SpatialAudioLab Virtual Sink

Test-driver notes for the SysVAD-based capture endpoint used by SpatialAudioLab. Start with the
[project README](../README.md) for architecture and requirements; this document focuses on driver
development, installation and recovery.

Before building, apply the versioned driver overlay from the repository root:

```powershell
.\tools\Initialize-SpatialAudioLab.ps1 -Component Driver
```

The initial prototype is pinned to Microsoft's SysVAD sample at commit
`2ee527bfeb0aeb6be11f0a8b6dce4011b358ce89` under `windows-driver-samples`.
The nested repository preserves its upstream history and MIT license.

The upstream HDMI endpoint already provides the two features needed for the first experiment:

- `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20` and `...DOLBY_MAT21` are advertised as
  8-channel, 192 kHz, 16-bit IEC 61937 carrier formats in
  `audio/sysvad/TabletAudioSample/hdmiwavtable.h`.
- Render data is copied by `CSaveData` to
  `%DriverData%\Audio_Samples\Sysvad\STREAM_HOST_*.wav`.

The installed prototype extends the x64 Release HDMI table with PCM 7.1 and MAT formats using the
Windows spatial-audio `0x63F` channel mask. Dolby Atmos Home Theater now opens MAT 2.1 Profile 3 and
`CSaveData` has captured the real renderer output. The current build also copies MAT 2.0/2.1 render
bytes into a 4 MiB nonpaged ring and exposes `\\.\DolbyDecoderMat` to administrators. The user-mode
probe reads it through bounded `READ`, `GET_STATS`, and `RESET` IOCTLs; decoding remains out of
kernel. Protocol v3 additionally captures the exact 12-channel PCM 7.1.4 format at 48 kHz and
reports its sample rate, channel mask, bit depth and block alignment. The endpoint uses mask
`0x2D63F`; PCM remains uninterpreted in kernel and is routed in user mode. A four-second MAT test
transferred 12,294,144 bytes with no sequence gaps or overflows.
The previously loaded package was `oem121.inf`, version `17.14.17.873`. Package `oem123.inf`,
version `2.55.40.516`, contains protocol v3 and PCM 7.1.4 and is selected pending a reboot. The
intermediate `oem122.inf` build is superseded. The previous post-reboot end-to-end MAT test decoded
199 bursts with no driver drops, malformed bursts, clipping, active starvation, clock drift, or
diagnostic WAV creation.

Kernel path `\DriverData` is a Windows-managed symbolic link. User mode must resolve it through the
`DriverData` environment variable. On this machine it is
`C:\Windows\System32\Drivers\DriverData`, not `C:\DriverData`:

```powershell
$captureRoot = Join-Path $env:DriverData 'Audio_Samples\Sysvad'
New-Item -ItemType Directory -Force $captureRoot | Out-Null
```

The script `tools\Capture-MatSpatialFixtures.ps1` resolves this path automatically. Set
`DoNotCreateDataFiles=0` under
`HKLM\SYSTEM\CurrentControlSet\Services\sysvad_componentizedaudiosample\Parameters` only when a
diagnostic WAV is required. The live ring does not depend on file output.
`tools\Install-SysvadCapture.ps1` sets the value to `1` by default; pass
`-EnableDiagnosticFileCapture` to set it to `0` for a later boot.

Do not run Battlefield while Windows is in Test Mode or while this test-signed driver is loaded. EA
Javelin AntiCheat can reject nonproduction drivers. Use Dolby Access or another unprotected Atmos
source for driver development, restore normal Secure Boot, and use a production-signed package or
external HDMI capture for the final Battlefield validation.

## Toolchain

From an elevated PowerShell in the repository root:

```powershell
.\tools\Install-WdkToolchain.ps1
.\tools\Get-DriverTestReadiness.ps1
```

The selected toolchain is Visual Studio 2022, Windows SDK build family 26100, and WDK
10.0.26100.6584. The SDK and WDK build number must match; their QFE numbers may differ.

## One-boot test

Prefer Windows Advanced startup option 7 for the first installation:

1. Settings -> System -> Recovery -> Advanced startup -> Restart now.
2. Troubleshoot -> Advanced options -> Startup Settings -> Restart.
3. Press `7` or `F7` for Disable driver signature enforcement.
4. Install the test-signed package and run the Dolby Access test in that boot.
5. Restart normally to restore signature enforcement.

Build the package serially (`/m:1`) and install only the base capture driver for the first test. The
toolchain and install scripts are idempotent:

```powershell
.\tools\Install-WdkToolchain.ps1
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  .\driver\windows-driver-samples\audio\sysvad\EndpointsCommon\EndpointsCommon.vcxproj `
  /m:1 /t:Build /p:Configuration=Release /p:Platform=x64 /p:TargetPlatformVersion=10.0.26100.0
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  .\driver\windows-driver-samples\audio\sysvad\Package\package.vcxproj `
  /m:1 /t:Build /p:Configuration=Release /p:Platform=x64 /p:TargetPlatformVersion=10.0.26100.0
.\tools\Install-SysvadCapture.ps1
```

The installer adds the WDK test certificate to the local machine Root and Trusted Publishers stores.
Remove both the device and that certificate after testing:

```powershell
.\tools\Uninstall-SysvadCapture.ps1
```

Persistent Test Mode is only needed for repeated development cycles. Check BitLocker first and use
`tools/Set-TestSigning.ps1`; it intentionally refuses to enable the setting while Secure Boot is on or
BitLocker protection is active.
