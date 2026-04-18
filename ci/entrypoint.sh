#!/bin/bash
# ------------------------------------------------------------------------------
# GitHub Actions self-hosted runner - entrypoint with keep-alive loop
# ------------------------------------------------------------------------------
set -euo pipefail

RUNNER_HOME="${RUNNER_HOME:-/home/runner/actions-runner}"
cd "${RUNNER_HOME}"

# -- Validate required env vars -------------------------------------------------
: "${GITHUB_URL:?  ERROR: GITHUB_URL must be set}"

RUNNER_NAME="${RUNNER_NAME:-$(hostname)}"
# RUNNER_ARCH is baked in at image build time; fall back to x64 if unset.
RUNNER_LABELS="${RUNNER_LABELS:-self-hosted,linux,${RUNNER_ARCH:-x64}}"
RUNNER_WORKDIR="${RUNNER_WORKDIR:-/home/runner/_work}"

echo "+==================================================+"
echo "|        GitHub Actions Runner - Starting up       |"
echo "+==================================================+"
echo "|  URL    : ${GITHUB_URL}"
echo "|  Name   : ${RUNNER_NAME}"
echo "|  Labels : ${RUNNER_LABELS}"
echo "+==================================================+"

# -- Register runner (only if not already configured) --------------------------
if [ ! -f ".runner" ]; then
    : "${RUNNER_TOKEN:?ERROR: RUNNER_TOKEN must be set for first-time registration}"
    echo "[entrypoint] Registering runner..."
    ./config.sh \
        --url        "${GITHUB_URL}" \
        --token      "${RUNNER_TOKEN}" \
        --name       "${RUNNER_NAME}" \
        --labels     "${RUNNER_LABELS}" \
        --work       "${RUNNER_WORKDIR}" \
        --unattended \
        --replace
    echo "[entrypoint] Registration complete."
else
    echo "[entrypoint] Runner already configured - skipping registration."
fi

# -- Graceful deregistration on container stop (SIGTERM / SIGINT) ---------------
cleanup() {
    echo ""
    echo "[entrypoint] Caught shutdown signal - deregistering runner..."
    if [ -n "${RUNNER_TOKEN:-}" ]; then
        ./config.sh remove --token "${RUNNER_TOKEN}" || true
    else
        echo "[entrypoint] WARNING: RUNNER_TOKEN not set - skipping deregistration. Runner may need manual cleanup on GitHub."
    fi
    echo "[entrypoint] Runner deregistered. Goodbye."
    exit 0
}
trap cleanup SIGTERM SIGINT SIGQUIT

# -- Keep-alive loop ------------------------------------------------------------
# Restarts the runner process automatically if it crashes.
# GitHub runner exits 0 on graceful stop (job finished / scale-down),
# and non-zero on unexpected crash - we restart in both cases unless
# the container itself receives a stop signal (handled above).

RESTART_DELAY=5   # seconds to wait before restarting after a crash
MAX_RESTARTS=0    # 0 = unlimited; set to e.g. 10 to cap retries
restart_count=0

while true; do
    echo "[entrypoint] Starting runner process... (restart #${restart_count})"
    
    # Run in background so our trap can fire
    ./run.sh &
    RUNNER_PID=$!
    
    wait "${RUNNER_PID}"
    EXIT_CODE=$?

    echo "[entrypoint] Runner process exited with code ${EXIT_CODE}."

    # Check restart cap
    if [ "${MAX_RESTARTS}" -gt 0 ] && [ "${restart_count}" -ge "${MAX_RESTARTS}" ]; then
        echo "[entrypoint] Reached max restart limit (${MAX_RESTARTS}). Exiting."
        exit "${EXIT_CODE}"
    fi

    restart_count=$((restart_count + 1))
    echo "[entrypoint] Restarting in ${RESTART_DELAY}s... (total restarts: ${restart_count})"
    sleep "${RESTART_DELAY}"
done
