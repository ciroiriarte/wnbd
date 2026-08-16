#!/usr/bin/env bash
#
# Copyright (c) 2026 Ciro Iriarte
# Licensed under LGPL-2.1 (see LICENSE)
#
# One-shot host-side driver for the self-hosted-driver-verifier workflow.
#
# Wraps the three manual steps into one command:
#   1. register an ON-DEMAND EPHEMERAL runner on the Windows lab VM
#      (tests/ci/setup-ephemeral-runner.ps1, run there via the QEMU guest agent),
#   2. dispatch the workflow for the requested mode,
#   3. wait for the run, report the result, and confirm the runner removed itself.
#
# It holds NO lab-specific values; supply them via env or a gitignored lab.env
# next to this script:
#   PVE_SSH    ssh target of the Proxmox host, e.g. root@10.2.0.210
#   WNBD_VM    VMID of the Windows runner VM, e.g. 150
#   WNBD_REPO  owner/repo, e.g. someone/wnbd   (gh may default to upstream!)
# Optional: RUNNER_LABEL (default wnbd-verifier), RUNNER_NAME (wnbd-verifier-eph),
#           SOAK_SECONDS / SOAK_THREADS / DISPATCHER_THREADS (soak/arm only).
#
# Usage:
#   tests/ci/dispatch-self-hosted.sh [soak|shutdown-hang-arm|shutdown-hang-verify]
# Default mode: shutdown-hang-verify.
#
# Preconditions by mode:
#   soak, shutdown-hang-arm  need Driver Verifier already enabled + the VM
#                            rebooted (tests/ci/enable-driver-verifier.ps1); the
#                            job's own step fails fast otherwise.
#   shutdown-hang-arm        reboots the VM; run '...-verify' AFTER it is back
#                            (a fresh ephemeral runner is registered each run).
#
# Requires on this host: gh (authenticated), ssh to PVE_SSH, python3, base64.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Optional local, gitignored config with the lab values.
[ -f "$SCRIPT_DIR/lab.env" ] && . "$SCRIPT_DIR/lab.env"

MODE="${1:-shutdown-hang-verify}"
: "${PVE_SSH:?set PVE_SSH (e.g. root@10.2.0.210) in env or tests/ci/lab.env}"
: "${WNBD_VM:?set WNBD_VM (e.g. 150) in env or tests/ci/lab.env}"
: "${WNBD_REPO:?set WNBD_REPO (e.g. owner/wnbd) in env or tests/ci/lab.env}"
RUNNER_LABEL="${RUNNER_LABEL:-wnbd-verifier}"
RUNNER_NAME="${RUNNER_NAME:-wnbd-verifier-eph}"
WORKFLOW="self-hosted-driver-verifier.yaml"
SETUP_PS="$SCRIPT_DIR/setup-ephemeral-runner.ps1"

case "$MODE" in
  soak|shutdown-hang-arm|shutdown-hang-verify) ;;
  *) echo "ERROR: unknown mode '$MODE'" >&2; exit 2 ;;
esac
[ -f "$SETUP_PS" ] || { echo "ERROR: $SETUP_PS not found" >&2; exit 2; }

log() { printf '\n=== %s ===\n' "$*"; }

# Run a PowerShell snippet on the VM via the QEMU guest agent, print its out-data.
psrun() {
  local ps="$1" tmo="${2:-120}" b64
  b64=$(python3 -c "import sys,base64;print(base64.b64encode(sys.argv[1].encode('utf-16-le')).decode())" "$ps")
  ssh "$PVE_SSH" "qm guest exec $WNBD_VM --timeout $tmo -- powershell.exe -NoProfile -EncodedCommand $b64" \
    | python3 -c "import sys,json
try:
    d=json.load(sys.stdin); print(d.get('out-data',''),end='')
    e=d.get('err-data') or ''
    if e and 'CLIXML' not in e[:20]: print(e[:400],file=sys.stderr)
except Exception as ex:
    print('psrun parse error:',ex,file=sys.stderr)"
}

wait_agent() {
  log "Waiting for the VM's guest agent"
  for _ in $(seq 1 24); do
    if ssh "$PVE_SSH" "qm agent $WNBD_VM ping" >/dev/null 2>&1; then
      echo "guest agent responding"; return 0
    fi
    sleep 10
  done
  echo "ERROR: guest agent did not respond (is VM $WNBD_VM running?)" >&2; exit 1
}

runner_online() {
  gh api "repos/$WNBD_REPO/actions/runners" \
    --jq "any(.runners[]?; .name==\"$RUNNER_NAME\" and .status==\"online\")" 2>/dev/null
}

wait_runner_online() {
  log "Waiting for the ephemeral runner to come online"
  for _ in $(seq 1 30); do
    [ "$(runner_online)" = "true" ] && { echo "runner online"; return 0; }
    sleep 6
  done
  echo "ERROR: runner did not register online within timeout" >&2; exit 1
}

wait_run_complete() {
  local run_id="$1"
  log "Waiting for run $run_id to complete"
  for _ in $(seq 1 60); do
    local st
    st=$(gh run view "$run_id" -R "$WNBD_REPO" --json status,conclusion \
          --jq '"\(.status) \(.conclusion)"' 2>/dev/null || echo "unknown")
    echo "  $st"
    case "$st" in completed*) return 0 ;; esac
    sleep 12
  done
  echo "ERROR: run $run_id did not complete within timeout" >&2; exit 1
}

# --- 1. VM up + latest setup script staged -----------------------------------
wait_agent

log "Staging setup-ephemeral-runner.ps1 on the VM"
SETUP_B64=$(base64 -w0 "$SETUP_PS")
psrun "New-Item -ItemType Directory -Force -Path C:\\wnbd-run | Out-Null; [IO.File]::WriteAllBytes('C:\\wnbd-run\\setup-ephemeral-runner.ps1',[Convert]::FromBase64String('$SETUP_B64')); 'staged'" 60

# --- 2. mint token + register ephemeral runner -------------------------------
log "Minting a registration token for $WNBD_REPO"
TOKEN=$(gh api -X POST "repos/$WNBD_REPO/actions/runners/registration-token" --jq '.token')
[ -n "$TOKEN" ] || { echo "ERROR: failed to mint registration token" >&2; exit 1; }

log "Registering the ephemeral runner (first run downloads ~200MB)"
psrun "& C:\\wnbd-run\\setup-ephemeral-runner.ps1 -Url 'https://github.com/$WNBD_REPO' -Token '$TOKEN' -Labels '$RUNNER_LABEL' -RunnerName '$RUNNER_NAME'" 300 || true
wait_runner_online

# --- 3. dispatch + wait ------------------------------------------------------
DISPATCH_ARGS=(-R "$WNBD_REPO" --ref main -f "mode=$MODE")
[ -n "${SOAK_SECONDS:-}" ]       && DISPATCH_ARGS+=(-f "soak_seconds=$SOAK_SECONDS")
[ -n "${SOAK_THREADS:-}" ]       && DISPATCH_ARGS+=(-f "soak_threads=$SOAK_THREADS")
[ -n "${DISPATCHER_THREADS:-}" ] && DISPATCH_ARGS+=(-f "dispatcher_threads=$DISPATCHER_THREADS")

# Record the newest run id before dispatch so we can identify the new one.
PREV_ID=$(gh run list -R "$WNBD_REPO" --workflow "$WORKFLOW" --limit 1 --json databaseId --jq '.[0].databaseId // 0' 2>/dev/null || echo 0)

log "Dispatching $WORKFLOW mode=$MODE"
gh workflow run "$WORKFLOW" "${DISPATCH_ARGS[@]}"

RUN_ID=""
for _ in $(seq 1 15); do
  cand=$(gh run list -R "$WNBD_REPO" --workflow "$WORKFLOW" --limit 1 --json databaseId --jq '.[0].databaseId // 0' 2>/dev/null || echo 0)
  if [ "$cand" != "$PREV_ID" ] && [ "$cand" != "0" ]; then RUN_ID="$cand"; break; fi
  sleep 3
done
[ -n "$RUN_ID" ] || { echo "ERROR: could not identify the dispatched run" >&2; exit 1; }
echo "run id: $RUN_ID  (https://github.com/$WNBD_REPO/actions/runs/$RUN_ID)"

wait_run_complete "$RUN_ID"

log "Result"
gh run view "$RUN_ID" -R "$WNBD_REPO" --json status,conclusion,jobs \
  --jq '"conclusion=\(.conclusion)", (.jobs[]? | select(.conclusion!="skipped") | "  \(.name): \(.conclusion)")'

CONCLUSION=$(gh run view "$RUN_ID" -R "$WNBD_REPO" --json conclusion --jq '.conclusion')

log "Runner state (ephemeral: should be gone)"
echo "registered runners: $(gh api "repos/$WNBD_REPO/actions/runners" --jq '.total_count')"

if [ "$MODE" = "shutdown-hang-arm" ]; then
  echo
  echo "NOTE: shutdown-hang-arm triggers an abrupt reboot. The VM is rebooting now;"
  echo "      once it is back, run: $0 shutdown-hang-verify"
fi

[ "$CONCLUSION" = "success" ] || { echo "run concluded: $CONCLUSION" >&2; exit 1; }
echo
echo "OK: $MODE completed successfully."
