#!/bin/bash
# ------------------------------------------------------------------------------
# build-and-run.sh - build and run a persistent GitHub Actions runner container
# ------------------------------------------------------------------------------
set -euo pipefail

# Always run relative to this script's directory so `podman build .` finds the
# Containerfile regardless of where the caller invokes the script from.
cd "$(dirname "$(realpath "$0")")" || exit 1

# -- Detect host architecture and map to GitHub Actions runner arch names -------
_raw_arch="$(uname -m)"
case "${_raw_arch}" in
  x86_64)          RUNNER_ARCH="x64"   ;;
  aarch64|arm64)   RUNNER_ARCH="arm64" ;;
  armv7l|armhf)    RUNNER_ARCH="arm"   ;;
  *)
    echo "WARNING: Unrecognised host arch '${_raw_arch}', defaulting RUNNER_ARCH to x64" >&2
    RUNNER_ARCH="x64"
    ;;
esac

IMAGE_NAME="gh-actions-runner-mingw32"
CONTAINER_NAME="gh-runner-mingw32"
VOLUME_NAME="gh-runner-data-mingw32"
WORKSPACE_ROOT="$(realpath ..)"
WORKSPACE_BASENAME="$(basename "${WORKSPACE_ROOT}")"
DEV_MOUNT_PATH="/home/runner/work/${WORKSPACE_BASENAME}"
# -- EDIT THESE ----------------------------------------------------------------
GITHUB_URL="https://github.com/Yabgu/ImageViewer"
RUNNER_TOKEN=""   # Only used ONCE during first registration
RUNNER_NAME="podman-runner-mingw32"
RUNNER_LABELS="self-hosted,mingw32,linux,${RUNNER_ARCH}"

# -------------------------------------------------------------------------------

CMD="${1:-help}"

case "${CMD}" in

  build)
    echo "Building image: ${IMAGE_NAME} (RUNNER_ARCH=${RUNNER_ARCH}) ..."
    podman build \
      --build-arg RUNNER_ARCH="${RUNNER_ARCH}" \
      -t "${IMAGE_NAME}" \
      .
    echo "Done."
    ;;

  run)
    # Create persistent volume if missing
    podman volume inspect "${VOLUME_NAME}" >/dev/null 2>&1 \
      || podman volume create "${VOLUME_NAME}"

    _volume_mountpoint="$(podman volume inspect -f '{{.Mountpoint}}' "${VOLUME_NAME}")"
    _runner_already_configured="false"
    if [[ -n "${_volume_mountpoint}" ]] && \
       [[ -f "${_volume_mountpoint}/actions-runner/.runner" || -f "${_volume_mountpoint}/actions-runner/.credentials" ]]; then
      _runner_already_configured="true"
    fi

    if [[ -z "${RUNNER_TOKEN}" && "${_runner_already_configured}" != "true" ]]; then
      echo "ERROR: RUNNER_TOKEN is not set. Edit build-and-run.sh before first registration." >&2
      exit 1
    fi

    echo "Starting container: ${CONTAINER_NAME} ..."

    podman run -d \
      --name "${CONTAINER_NAME}" \
      --restart unless-stopped \
      -v "${VOLUME_NAME}:/home/runner" \
      -e RUN_MODE="run" \
      -e GITHUB_URL="${GITHUB_URL}" \
      -e RUNNER_TOKEN="${RUNNER_TOKEN}" \
      -e RUNNER_NAME="${RUNNER_NAME}" \
      -e RUNNER_LABELS="${RUNNER_LABELS}" \
      "${IMAGE_NAME}"

    echo "Runner started. Use './build-and-run.sh logs' to follow output."
    ;;

  dev)
    echo "Starting interactive dev container with workspace mounted at ${DEV_MOUNT_PATH} ..."

    podman run --rm -it \
      --name "${CONTAINER_NAME}-dev" \
      --userns=keep-id \
      --user "$(id -u):$(id -g)" \
      -e RUN_MODE="dev" \
      -e GITHUB_WORKSPACE="${DEV_MOUNT_PATH}" \
      -v "${WORKSPACE_ROOT}:${DEV_MOUNT_PATH}:rw" \
      -w "${DEV_MOUNT_PATH}" \
      "${IMAGE_NAME}"
    ;;

  stop)
    echo "Stopping container: ${CONTAINER_NAME} ..."
    podman stop "${CONTAINER_NAME}"
    podman rm "${CONTAINER_NAME}"
    echo "Container removed."
    ;;

  logs)
    podman logs -f "${CONTAINER_NAME}"
    ;;

  restart)
    "$0" stop
    "$0" run
    ;;

  help|*)
    echo "Usage: $0 {build|run|dev|stop|logs|restart}"
    ;;
esac
