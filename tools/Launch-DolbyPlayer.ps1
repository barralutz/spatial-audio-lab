[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$InputFile = '',

    [ValidateRange(0.0, 1.0)]
    [double]$Gain = 1.0,

    [ValidateRange(-5000, 5000)]
    [double]$AvDelayMilliseconds = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Show-PlayerError([string]$Message) {
    [Windows.Forms.MessageBox]::Show(
        $Message, 'DolbyPlayer Atmos 7.1.4',
        [Windows.Forms.MessageBoxButtons]::OK,
        [Windows.Forms.MessageBoxIcon]::Error) | Out-Null
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-PcmBridge {
    @(Get-CimInstance Win32_Process -Filter "Name='dolby-probe.exe'" |
        Where-Object { $_.CommandLine -match '(?i)\blive-pcm-layout\b' }).Count -ne 0
}

function Start-ElevatedLauncher([string]$MediaPath) {
    $gainText = $Gain.ToString([Globalization.CultureInfo]::InvariantCulture)
    $delayText = $AvDelayMilliseconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    $arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden ' +
        "-File `"$PSCommandPath`" -InputFile `"$MediaPath`" " +
        "-Gain $gainText -AvDelayMilliseconds $delayText"
    Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs | Out-Null
}

function New-ProgressWindow {
    $form = [Windows.Forms.Form]::new()
    $form.Text = 'DolbyPlayer Atmos 7.1.4'
    $form.ClientSize = [Drawing.Size]::new(380, 94)
    $form.StartPosition = [Windows.Forms.FormStartPosition]::CenterScreen
    $form.FormBorderStyle = [Windows.Forms.FormBorderStyle]::FixedDialog
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true

    $label = [Windows.Forms.Label]::new()
    $label.AutoSize = $true
    $label.Location = [Drawing.Point]::new(18, 16)
    $label.Text = 'Preparando audio PCM 7.1.4...'
    $form.Controls.Add($label)

    $progress = [Windows.Forms.ProgressBar]::new()
    $progress.Location = [Drawing.Point]::new(18, 47)
    $progress.Size = [Drawing.Size]::new(344, 20)
    $progress.Style = [Windows.Forms.ProgressBarStyle]::Marquee
    $progress.MarqueeAnimationSpeed = 24
    $form.Controls.Add($progress)
    $form
}

try {
    if ([string]::IsNullOrWhiteSpace($InputFile)) {
        $dialog = [Windows.Forms.OpenFileDialog]::new()
        $dialog.Title = 'Abrir pelicula Atmos'
        $dialog.Filter = 'Peliculas Matroska (*.mkv;*.mka)|*.mkv;*.mka|Todos los archivos|*.*'
        $dialog.Multiselect = $false
        if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return }
        $InputFile = $dialog.FileName
        $dialog.Dispose()
    }

    $mediaPath = [IO.Path]::GetFullPath($InputFile)
    if (-not (Test-Path $mediaPath -PathType Leaf)) {
        throw "No se encontro el archivo:`n$mediaPath"
    }
    if (Get-Process 'dolby-player' -ErrorAction SilentlyContinue) {
        throw 'DolbyPlayer ya esta reproduciendo una pelicula.'
    }

    if (-not (Test-PcmBridge) -and -not (Test-Administrator)) {
        Start-ElevatedLauncher $mediaPath
        return
    }

    $progress = New-ProgressWindow
    $progress.Show()
    [Windows.Forms.Application]::DoEvents()
    try {
        & (Join-Path $PSScriptRoot 'Ensure-LivePcm714.ps1')
    } finally {
        $progress.Close()
        $progress.Dispose()
    }

    & (Join-Path $PSScriptRoot 'Start-DolbyPlayer.ps1') `
        -InputFile $mediaPath -Gain $Gain -AvDelayMilliseconds $AvDelayMilliseconds `
        -SkipBridgeSetup
} catch {
    Show-PlayerError "$_"
}
