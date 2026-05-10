# Copilot instructions — CI dev image

Notes for contributors and Copilot:

- Running `ci/build-and-run.sh build` builds the dev image used by the GitHub Actions runner container (image name: `gh-actions-runner-mingw32`).
- Building the image requires a prebuilt AUR package: `llvm-mingw`. Ensure a prebuilt `llvm-mingw` is available before building the image.
- Currently a prebuilt `llvm-mingw` binary is present in the repository (user-provided). Add reproducible build instructions for `llvm-mingw` later (TODO).
- How to start / enter the dev image:
  - Interactive dev container: run `ci/build-and-run.sh dev` (or set `RUN_MODE="dev"`).
  - Detached/background runner container: run `ci/build-and-run.sh run` (script uses `podman run -d`).
- Useful names used by the scripts:
  - Image: `gh-actions-runner-mingw32`
  - Container: `gh-runner-mingw32`
  - Volume: `gh-runner-data-mingw32`
  - Dev mount path (inside dev container): `/home/runner/work/<workspace_basename>`

Next actions:
1. Fix build failures observed when building inside the image (inspect Containerfile/Containerfile context and test interactively).
2. Add automated instructions to prebuild `llvm-mingw` or include a reproducible build step in CI.
3. Verify container startup and interactive workflows across host environments.

(Added on 2026-05-10)
