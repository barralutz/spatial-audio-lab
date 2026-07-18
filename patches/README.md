# Dependency patches

SpatialAudioLab keeps third-party projects as submodules pinned to public commits. The active
source overlay required by the current prototype is:

- `windows-driver-samples/SpatialAudioLab.patch`: SysVAD MAT/DTS:X/PCM capture ring and 7.1.4
  endpoint formats.

Cinema obtains its custom Cavern changes directly from the public
[`barralutz/Cavern`](https://github.com/barralutz/Cavern) fork and its `spatial-audio-lab` branch.
The submodule gitlink pins the exact revision used by SpatialAudioLab. The fork preserves Cavern's
upstream history and license.

`Cavern/SpatialAudioLab.patch` is retained as a compatibility snapshot for older checkouts that
still point at the original upstream revision. On the current fork,
`tools/Initialize-SpatialAudioLab.ps1` detects those changes as already present and does not apply
them again.

The initialization script applies each missing overlay only when necessary. It supports Git for
Windows and falls back to Git through WSL. Applying the SysVAD patch intentionally leaves that
submodule worktree modified while preserving its public upstream history and license.
