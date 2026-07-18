# SpatialAudioLab Portable Developer Preview Roadmap

**Design:** `docs/superpowers/specs/2026-07-17-portable-developer-preview-design.md`

The approved design is split into four implementation plans. Execute them in order because every
later plan consumes public interfaces and artifacts from the previous one.

1. `2026-07-17-portable-runtime-foundation.md`
   - Shared runtime-path and profile libraries.
   - Versioned profiles, presets, endpoint identity, and variable-layout native rendering.
   - Studio and Cinema run without a repository or WSL.
2. `2026-07-17-first-run-onboarding.md`
   - First-run wizard, endpoint probing, route proposal, channel tones, and profile management.
3. `2026-07-17-setup-driver-lifecycle.md`
   - Setup application, resumable advanced-startup flow, driver lifecycle, provider transactions,
     repair, rollback, and diagnostics.
4. `2026-07-17-release-packaging.md`
   - MIT and third-party notices, pinned dependencies, deterministic staging, portable ZIP, Inno
     Setup installer, package tests, and release validation.

Each plan must be complete and green before the next begins. Existing source-tree development
scripts remain supported until the release-packaging plan replaces their user-facing role.

