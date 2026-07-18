[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]]$Candidates
)

$available = @($Candidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    ForEach-Object { Get-Item -LiteralPath $_ } |
    Sort-Object LastWriteTimeUtc -Descending)
if ($available.Count -eq 0) {
    throw "SpatialAudioLab Engine was not found in: $($Candidates -join ', ')"
}

$available[0].FullName
