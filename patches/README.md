# Dependency patches

SpatialAudioLab keeps third-party projects as submodules pinned to public upstream commits. Two
squashed patches contain the source changes required by the current prototype:

- `windows-driver-samples/SpatialAudioLab.patch`: SysVAD MAT/DTS:X/PCM capture ring and 7.1.4
  endpoint formats.
- `Cavern/SpatialAudioLab.patch`: incremental E-AC-3 JOC rendering fixes used by Cinema.

`tools/Initialize-SpatialAudioLab.ps1` applies each patch only when necessary. It supports Git for
Windows and falls back to Git through WSL. Applying the patches intentionally leaves the submodule
worktrees modified while preserving their public upstream history and licenses.
