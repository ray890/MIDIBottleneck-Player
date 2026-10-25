#!/bin/bash
# Linux check (Milestone 8, stage 2): runs every portable test program and the v2 parity check against the committed
# data, from the repository root. Usage: v2/tools/linux-check.sh BUILD_DIR [quick] [gtk]
#   BUILD_DIR  the CMake build directory (see v2/BUILDING.md, "Linux")
#   quick      skips the 3,956 engine runs of the parity check (every other part still runs)
# The v1 drift check (regenerating v1's traces with the .NET harness) is Windows-only: tools/parity.ps1. This script
# checks tier 1 (v2 against the committed v1 traces and engine digests, every difference explained by a rule) and
# tier 2 (v2 against its own recorded baseline, exactly) with the same program the Windows builds use (midibn-parity).
# Ends with "LINUX-CHECK: PASS" or "LINUX-CHECK: FAIL" (exit code 0 / 1). Nothing here opens a MIDI device.
#   gtk        also runs the GTK player's scripted check (needs midibn-gtk built and xvfb-run; the player runs only under a
#              virtual X server, never on a real screen, and only on output None: no MIDI port is opened, no sound)
build=${1:?usage: linux-check.sh BUILD_DIR [quick] [gtk]}
quick=
gtk=
for argument in "${@:2}"; do
  case $argument in
    quick) quick=quick ;;
    gtk) gtk=gtk ;;
    *) echo "unknown argument: $argument"; exit 2 ;;
  esac
done
cd "$(dirname "$0")/../.." || exit 2
failures=0
run() {   # name args...
  local name=$1; shift
  local start=$SECONDS
  local log
  log=$(mktemp)
  "$build/$name" "$@" > "$log" 2>&1
  local rc=$?
  printf '%-28s %s  %4d s  %s\n' "$name" "$([ $rc -eq 0 ] && echo PASS || echo FAIL)" $((SECONDS - start)) "$(grep -E '^(RESULT|PASS:|FAIL:|UI TESTS|CHANNELS APP TESTS|midibn-analysis-app-tests:)' "$log" | tail -n 1 | cut -c1-110)"
  if [ $rc -ne 0 ]; then failures=$((failures + 1)); grep -E '^FAIL' "$log" | head -5 | cut -c1-200; fi
  rm -f "$log"
}
run midibn-tests
run midibn-ui-tests v2/harness/expected/ui
run midibn-channels-app-tests v2/harness/expected/channels-ui
run midibn-app-tests v2/harness/fixtures
run midibn-analysis-app-tests v2/harness/fixtures v2/harness/expected/analysis-ui
run midibn-routing-app-tests v2/harness/fixtures v2/harness/expected/routing-ui v2/harness/expected-v2-ui/routing-ui/explained-states.txt
run midibn-engine-tests
[ -x "$build/midibn-linux-platform-tests" ] && run midibn-linux-platform-tests
[ -x "$build/midibn-alsa-output-tests" ] && run midibn-alsa-output-tests
parity_args=(--fixtures v2/harness/fixtures --expected v2/harness/expected --baseline v2/harness/expected-v2 --scenarios v2/harness/engine-scenarios.txt)
[ "$quick" = quick ] && parity_args+=(--no-engine)
run midibn-parity "${parity_args[@]}"
if [ "$quick" != quick ]; then
  run midibn-engine-harness --fixtures v2/harness/fixtures --scenarios v2/harness/engine-scenarios.txt --expected v2/harness/expected/engine --rules v2/harness/expected/differences-from-v1.txt --baseline v2/harness/expected-v2/engine
fi
if [ "$gtk" = gtk ]; then
  # The GTK player under Xvfb (xvfb-run -a picks a free display and sets DISPLAY for the program).
  gtk_run() {   # name args...
    local name=$1; shift
    local start=$SECONDS log
    log=$(mktemp)
    NO_AT_BRIDGE=1 GDK_BACKEND=x11 timeout 120 xvfb-run -a -s "-screen 0 1920x1080x24" "$build/midibn-gtk" --strict "$@" > "$log" 2>&1
    local rc=$?
    printf '%-28s %s  %4d s  %s\n' "$name" "$([ $rc -eq 0 ] && echo PASS || echo FAIL)" $((SECONDS - start)) "$(grep -E '^SMOKE (PASS|FAIL)' "$log" | tail -n 1)"
    if [ $rc -ne 0 ]; then failures=$((failures + 1)); grep -E 'SMOKE FAIL|midibn-gtk:|CRITICAL|WARNING' "$log" | head -8 | cut -c1-200; fi
    rm -f "$log"
  }
  if [ -x "$build/midibn-gtk" ] && command -v xvfb-run > /dev/null; then
    fixtures=v2/harness/fixtures
    gtk_run "midibn-gtk smoke" --smoke --open "$fixtures/models-dense.mid"
    gtk_run "midibn-gtk routing dialog" --open "$fixtures/routing-lanes.mid" --show routing --auto-close-ms 2500
    gtk_run "midibn-gtk about+large" --open "$fixtures/models-dense.mid" --force-large-warning --auto-close-ms 1500
  else
    echo "midibn-gtk                   SKIP  (midibn-gtk or xvfb-run is not available)"
    failures=$((failures + 1))
  fi
fi
if [ $failures -eq 0 ]; then echo "LINUX-CHECK: PASS"; exit 0; fi
echo "LINUX-CHECK: FAIL ($failures)"
exit 1
