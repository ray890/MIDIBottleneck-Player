#!/bin/bash
# Linux self-test package (Milestone 8, stage 6): builds the portable programs for distribution and packs them with the
# committed test data, two launchers and a one-page README for a tester who runs a real Linux PC.
#
# One command, from Windows:  wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/Claude/MIDI\ Player/v2/tools/package-linux-selftest.sh
# (also fine on any Linux with CMake 3.20+, Ninja, g++ and libasound2-dev; libgtk-3-dev adds the GTK player when it exists)
#
# Output (default: in the checkout this script lives in; MIDIBN_PKG_OUT overrides): v2/out/linux-selftest/MIDIBottleneck-linux-selftest.tar.gz and .sha256
# The build tree goes in the Linux file system: $MIDIBN_PKG_BUILD (default /root/build/P11-LPKG/pkg-build when /root exists,
# else ~/.cache/midibn-pkg-build). The GTK player (target midibn-gtk) is included automatically when the build makes it.
set -u
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${MIDIBN_PKG_BUILD:-$([ -d /root ] && [ -w /root ] && echo /root/build/linux-selftest-pkg/pkg-build || echo "$HOME/.cache/midibn-pkg-build")}
outdir=${MIDIBN_PKG_OUT:-$repo/v2/out/linux-selftest}   # MIDIBN_PKG_OUT: write the package somewhere else (a shared folder)
name=MIDIBottleneck-linux-selftest
stage=$(dirname "$build")/stage
top=$stage/$name                 # the package: README.txt, start-player.sh, "MIDIBottleneck Player/", "self-test/"
pkg=$top/self-test               # the checks and measurements (no sound)
log=$(dirname "$build")/package.log
mkdir -p "$build" "$outdir" "$stage" || exit 2
rm -rf "$top"; mkdir -p "$pkg" "$top/MIDIBottleneck Player" || exit 2
# Where a program ends up in the package: the player has its own folder and name, the tools are in self-test/.
bp() { if [ "$1" = midibn-gtk ]; then echo "$top/MIDIBottleneck Player/MIDIBottleneck Player"; else echo "$pkg/$1"; fi; }
fail() { echo "PACKAGE FAILED: $*" >&2; exit 1; }

echo "[1/5] configure + build (Release, static libstdc++/libgcc) in $build"
cmake -S "$repo/v2" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DMIDIBN_PORTABLE_LINUX=ON > "$log" 2>&1 || { tail -20 "$log"; fail "cmake"; }
cmake --build "$build" >> "$log" 2>&1 || { tail -30 "$log"; fail "build"; }

echo "[2/5] collect programs and data"
tools="midibn-tests midibn-ui-tests midibn-channels-app-tests midibn-app-tests midibn-analysis-app-tests midibn-routing-app-tests
midibn-engine-tests midibn-linux-platform-tests midibn-alsa-output-tests midibn-parity midibn-engine-harness
midibn-linux-capability midibn-alsa-ports midibn-alsa-send-check"
gtk=midibn-gtk   # the player is required: the package is for testing it
for t in $tools $gtk; do
  [ -x "$build/$t" ] || fail "missing program $t (is libasound2-dev / libgtk-3-dev installed?)"
  cp "$build/$t" "$(bp "$t")" || fail "copy $t"
  strip "$(bp "$t")" || fail "strip $t"
done
h=$repo/v2/harness
d=$pkg/v2/harness
mkdir -p "$d" "$pkg/v2/harness/expected-v2-ui/routing-ui" || exit 2
cp -r "$h/fixtures" "$h/expected" "$h/expected-v2" "$d/" || fail "copy data"
cp "$h/engine-scenarios.txt" "$d/" || fail "copy scenarios"
cp "$h/expected-v2-ui/routing-ui/explained-states.txt" "$d/expected-v2-ui/routing-ui/" || fail "copy explained states"
# the 3 files under expected\ that no Linux program reads (the Windows-only capability probe data) stay; everything else is needed.

echo "[3/5] scan symbol versions and libraries"
# Highest GLIBC_ version required by any program, the C++ runtime symbol check (must be none: statically linked),
# and the union of the shared libraries (NEEDED).
maxglibc=0.0
: > "$stage/glibc-by-program.txt"
for t in $tools $gtk; do
  v=$(objdump -T "$(bp "$t")" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
  [ -z "$v" ] && v=none
  printf '  %-30s %s\n' "$t" "$v" >> "$stage/glibc-by-program.txt"
  if [ "$v" != none ] && [ "$(printf '%s\n%s\n' "$maxglibc" "$v" | sort -V | tail -n 1)" = "$v" ]; then maxglibc=$v; fi
done
cxxleft=$(for t in $tools $gtk; do objdump -T "$(bp "$t")" | grep -E 'GLIBCXX_|CXXABI_|GCC_[0-9]' ; done | wc -l)
[ "$cxxleft" = 0 ] || fail "a program still needs libstdc++/libgcc symbols ($cxxleft), not statically linked"
for t in $tools $gtk; do readelf -d "$(bp "$t")" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; done | sort -u > "$stage/needed.txt"
# Library name -> which programs need it (the GTK list is long; the tools need only libc, libm, libasound and libpthread)
for t in $tools $gtk; do readelf -d "$(bp "$t")" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sed "s|^|$t |"; done > "$stage/needed-by-program.txt"
toolneeded=$(for t in $tools; do readelf -d "$pkg/$t" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; done | sort -u | tr '\n' ' ')
gtkneeded=
[ -n "$gtk" ] && gtkneeded=$(readelf -d "$(bp "$gtk")" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u | tr '\n' ' ')
echo "      highest GLIBC symbol version: $maxglibc"

# Distributions whose C library is at least that new (glibc version of each release; the list is a reference table).
distros=$(cat <<'T'
2.28|Debian 10 "Buster", RHEL / AlmaLinux / Rocky 8
2.31|Ubuntu 20.04, Linux Mint 20, Debian 11 "Bullseye", openSUSE Leap 15.3-15.5
2.33|Fedora 34
2.34|RHEL / AlmaLinux / Rocky 9, Fedora 35
2.35|Ubuntu 22.04, Linux Mint 21, Fedora 36, Pop!_OS 22.04
2.36|Debian 12 "Bookworm"
2.37|Fedora 38, Ubuntu 23.04
2.38|Ubuntu 23.10, Fedora 39
2.39|Ubuntu 24.04, Linux Mint 22, Fedora 40
2.40|Fedora 41
2.41|Fedora 42, Debian 13 "Trixie"
T
)
okdistros=
firstok=
while IFS='|' read -r gv names; do
  if [ "$(printf '%s\n%s\n' "$maxglibc" "$gv" | sort -V | tail -n 1)" = "$gv" ]; then   # gv >= required
    [ -z "$firstok" ] && firstok=$gv
    okdistros="$okdistros  glibc $gv:  $names
"
  fi
done <<< "$distros"
olddistros=
while IFS='|' read -r gv names; do
  if [ "$(printf '%s\n%s\n' "$maxglibc" "$gv" | sort -V | tail -n 1)" != "$gv" ]; then olddistros="$olddistros$names; "; fi
done <<< "$distros"
olddistros=${olddistros%; }
[ -n "$okdistros" ] || fail "no distribution in the table has glibc $maxglibc; extend the table"

echo "[4/5] launchers and README"
cat > "$pkg/selftest-lib.sh" <<'LIB'
# Shared by run-selftest.sh and run-selftest-full.sh (do not run this file directly).
here=$(cd "$(dirname "$0")" && pwd)
cd "$here" || exit 2
if ! touch .write-test 2>/dev/null; then
  echo "This folder is not writable, so the result files cannot be saved."
  echo "Copy the whole folder somewhere you own (for example your home folder) and run it from there."
  exit 2
fi
rm -f .write-test
for p in midibn-*; do [ -f "$p" ] && chmod +x "$p" 2>/dev/null; done
H=v2/harness
mkdir -p logs
RES=$RESULT_FILE
started=$(date '+%Y-%m-%d %H:%M:%S')
t0=$SECONDS
failures=0
{
  echo "MIDIBottleneck Player v2 - Linux self-test ($MODE run)"
  echo "Started: $started"
  echo "Computer: $(uname -srm)"
  . /etc/os-release 2>/dev/null && echo "Distribution: ${PRETTY_NAME:-unknown}"
  echo "Package: $(cat PACKAGE-INFO.txt 2>/dev/null | head -n 1)"
  echo
} > "$RES"

say() { echo "$*" | tee -a "$RES"; }

step() {   # step NAME PROGRAM ARGS...   (counts toward OVERALL)
  local name=$1 prog=$2; shift 2
  local s=$SECONDS rc
  if [ ! -x "./$prog" ]; then say "$(printf '%-34s FAIL  (program missing)' "$name")"; failures=$((failures + 1)); return; fi
  "./$prog" "$@" > "logs/$name.txt" 2>&1
  rc=$?
  local last
  last=$(grep -E '^(RESULT|PASS:|FAIL:|UI TESTS|CHANNELS APP TESTS|midibn-analysis-app-tests:|ALSA)' "logs/$name.txt" | tail -n 1 | cut -c1-100)
  if [ $rc -eq 0 ]; then
    say "$(printf '%-34s PASS  %4d s  %s' "$name" $((SECONDS - s)) "$last")"
  else
    say "$(printf '%-34s FAIL  %4d s  (exit code %d)' "$name" $((SECONDS - s)) $rc)"
    failures=$((failures + 1))
    { echo "--- first failing lines of $name ---"; grep -E 'FAIL|rror|mismatch' "logs/$name.txt" | head -n 15 | cut -c1-220; echo; } >> selftest-details.txt
  fi
}
: > selftest-details.txt

echo "MIDIBottleneck Player v2 - Linux self-test ($MODE run)"
echo "No sound is made and no MIDI device is used by these checks."
echo

step unit-tests-core midibn-tests
step unit-tests-display midibn-ui-tests $H/expected/ui
step unit-tests-channels midibn-channels-app-tests $H/expected/channels-ui
step unit-tests-main-window midibn-app-tests $H/fixtures
step unit-tests-analysis midibn-analysis-app-tests $H/fixtures $H/expected/analysis-ui
step unit-tests-routing midibn-routing-app-tests $H/fixtures $H/expected/routing-ui $H/expected-v2-ui/routing-ui/explained-states.txt
step unit-tests-engine midibn-engine-tests
step unit-tests-linux-platform midibn-linux-platform-tests
step unit-tests-alsa-output midibn-alsa-output-tests
parity_args="--fixtures $H/fixtures --expected $H/expected --baseline $H/expected-v2 --scenarios $H/engine-scenarios.txt"
if [ "$MODE" = quick ]; then
  step v2-baseline-and-v1-traces midibn-parity $parity_args --no-engine
else
  step v2-baseline-and-engine-runs midibn-parity $parity_args
  step engine-harness midibn-engine-harness --fixtures $H/fixtures --scenarios $H/engine-scenarios.txt --expected $H/expected/engine --rules $H/expected/differences-from-v1.txt --baseline $H/expected-v2/engine
fi

# Information about this computer (not part of OVERALL).
s=$SECONDS
if [ "$MODE" = quick ]; then ./midibn-linux-capability --quick > capability-report.txt 2>&1; else ./midibn-linux-capability > capability-report.txt 2>&1; fi
say "$(printf '%-34s done  %4d s  capability-report.txt (information, not part of OVERALL)' capability-report $((SECONDS - s)))"
s=$SECONDS
./midibn-alsa-ports > alsa-ports.txt 2>&1
say "$(printf '%-34s done  %4d s  alsa-ports.txt (information, not part of OVERALL)' port-list $((SECONDS - s)))"

if [ "$MODE" = quick ]; then
  echo
  echo "Optional: silent send check"
  echo "  This sends ONLY silent controller messages (no notes, so no sound) to one MIDI port you choose,"
  echo "  and measures how fast the port accepts them. Choose the Qsynth/FluidSynth port (README step 2), or Enter to skip."
  echo "  Safe choices: a software synthesizer you started yourself (FluidSynth, TiMidity++) or \"Midi Through\"."
  echo "  Do not choose a port of a hardware keyboard or synthesizer you do not want to touch."
  echo
  echo "Ports found:"
  sed 's/^/    /' alsa-ports.txt
  echo
  port=
  if ! grep -qE '^[0-9]+:[0-9]+ ' alsa-ports.txt; then
    echo "(No MIDI port is available, so the send check is skipped.)"
  elif [ -t 0 ]; then
    read -r -p "Type a port as shown on the left (for example 14:0), or just press Enter to skip: " port
  fi
  if [[ "$port" =~ ^[0-9]+:[0-9]+$ ]]; then
    s=$SECONDS
    ./midibn-alsa-send-check "$port" --seconds 3 > alsa-send-check.txt 2>&1
    rc=$?
    say "$(printf '%-34s %s  %4d s  alsa-send-check.txt (port %s; information, not part of OVERALL)' send-check "$([ $rc -eq 0 ] && echo done || echo FAIL)" $((SECONDS - s)) "$port")"
  else
    echo "Send check skipped." > alsa-send-check.txt
    say "$(printf '%-34s skipped (not run)' send-check)"
  fi
fi

echo | tee -a "$RES"
total=$((SECONDS - t0))
say "Elapsed: $total s"
if [ $failures -eq 0 ]; then overall=PASS; else overall=FAIL; fi
say "OVERALL: $overall"
echo
echo "Please send back these files from the folder $here:"
echo "  $RES"
echo "  capability-report.txt"
echo "  alsa-ports.txt"
[ "$MODE" = quick ] && echo "  alsa-send-check.txt   (only if you ran the send check)"
if [ "$overall" = FAIL ]; then echo "  selftest-details.txt  and the folder logs"; fi
[ "$overall" = PASS ]
LIB
cat > "$pkg/run-selftest.sh" <<'L'
#!/bin/bash
# Quick self-test: unit tests, v2 baseline and v1 trace comparison, capability report, MIDI port list. A few minutes.
MODE=quick
RESULT_FILE=selftest-result.txt
. "$(dirname "$0")/selftest-lib.sh"
L
cat > "$pkg/run-selftest-full.sh" <<'L'
#!/bin/bash
# Full self-test: everything in the quick run plus the full parity check (all engine runs) and the engine harness.
MODE=full
RESULT_FILE=selftest-result-full.txt
. "$(dirname "$0")/selftest-lib.sh"
L
chmod +x "$pkg/run-selftest.sh" "$pkg/run-selftest-full.sh"

cat > "$top/start-player.sh" <<'L'
#!/bin/bash
# Starts the MIDIBottleneck Player (the program is in the folder "MIDIBottleneck Player").
here=$(cd "$(dirname "$0")" && pwd)
chmod +x "$here/MIDIBottleneck Player/MIDIBottleneck Player" 2>/dev/null
exec "$here/MIDIBottleneck Player/MIDIBottleneck Player" "$@"
L
chmod +x "$top/start-player.sh"
cat > "$top/MIDIBottleneck Player/README-player.txt" <<'L'
This folder holds the MIDIBottleneck Player (early Linux test version).
Start it with:  bash start-player.sh  (one folder up), or run "MIDIBottleneck Player" here.
The steps are in README.txt at the top of the package.
L
cat > "$pkg/PACKAGE-INFO.txt" <<EOF
MIDIBottleneck Player v2 Linux self-test package, built $(date '+%Y-%m-%d %H:%M') on $(. /etc/os-release; echo "$PRETTY_NAME")
Highest glibc symbol version needed: $maxglibc
EOF
cat > "$pkg/TECHNICAL-DETAILS.txt" <<EOF
MIDIBottleneck Player v2 Linux package - technical details (for us; you do not need to read this)
$(head -n 1 "$pkg/PACKAGE-INFO.txt")
Highest glibc symbol version required: $maxglibc. Distributions with at least that glibc:
$okdistros  Older than that (not supported): ${olddistros:-none in the table}
  To see your glibc version: ldd --version
Highest glibc symbol version, per program:
$(cat "$stage/glibc-by-program.txt")
C++ runtime: linked statically (no libstdc++ or libgcc needed).
Shared libraries needed by the test programs: $toolneeded
Shared libraries needed by the player: ${gtkneeded}
Programs in self-test/: midibn-tests, midibn-ui-tests, midibn-channels-app-tests, midibn-app-tests,
  midibn-analysis-app-tests, midibn-routing-app-tests, midibn-engine-tests, midibn-linux-platform-tests,
  midibn-alsa-output-tests, midibn-parity, midibn-engine-harness, midibn-linux-capability, midibn-alsa-ports,
  midibn-alsa-send-check
EOF
cat > "$top/README.txt" <<EOF
MIDIBOTTLENECK PLAYER v2 - LINUX TEST (one page)
================================================
Two parts: the PLAYER (folder "MIDIBottleneck Player", step 4) and the
checks (folder "self-test", step 3). Nothing is installed; run as your normal
user, not as root. The checks make no sound; the player does (your synth).

1. UNPACK (once)    tar -xzf MIDIBottleneck-linux-selftest.tar.gz
                    cd MIDIBottleneck-linux-selftest

2. MAKE QSYNTH'S MIDI PORT APPEAR (keep Qsynth open until step 4 is done)
   a) Start Qsynth. Open Setup, tab "MIDI": tick "Enable MIDI input" and set
      "MIDI driver" to "alsa_seq". Press OK (it restarts the engine).
      Also check Setup > Soundfonts: a SoundFont must be loaded, or step 4
      will be silent.
   b) In a terminal run:   aconnect -o
      A client like  "client 128: 'FLUID Synth (1234)'"  must be listed;
      its port is then 128:0. (No aconnect? Run: self-test/midibn-alsa-ports)
      Not listed? Send us the output of aconnect -o and a screenshot of the
      MIDI tab, and skip the port parts below.

3. SELF-TEST (1-3 minutes, silent)
      cd self-test
      bash run-selftest.sh
   At the end it lists the ports and asks for a port for a silent send check
   (only silent controller messages): type the Qsynth port, e.g. 128:0, and
   press Enter. Enter alone skips. Optional long test (8-20 minutes):
      bash run-selftest-full.sh
   Send back from the self-test folder: selftest-result.txt (and
   selftest-result-full.txt), capability-report.txt, alsa-ports.txt,
   alsa-send-check.txt. If it says FAIL also send selftest-details.txt and
   the folder logs.

4. THE PLAYER - first real sound test on Linux (back in the package folder)
      bash start-player.sh
   a) Click "Open MIDI..." and pick any MIDI file you like.
   b) In the "File and output" group, open the output list and choose the
      Qsynth entry (the same port as in step 2, e.g. 128:0).
   c) Click Play. You should hear the music through Qsynth. Also try Pause,
      Stop, the +5 sec button and dragging the position.
   d) Send us: a screenshot while it plays, one of any odd-looking or
      broken window, and a few words: was there sound, any stutter, stuck
      notes after Stop, error messages, anything slow or strange?
   (Press F1 in the player for its Help. The first version has no
   KDMAPI/Direct options; those are Windows only.)

NEEDS: 64-bit Linux, a desktop with GTK 3 and the ALSA library (normal
desktop installs have both), glibc $maxglibc or newer (roughly Ubuntu 22.04,
Debian 12, Fedora 36 or newer). If a program says a library is missing, send
us the exact message. Details for us: self-test/TECHNICAL-DETAILS.txt
EOF

echo "[5/5] pack"
mkdir -p "$outdir"
rm -f "$outdir/$name.tar.gz" "$outdir/$name.tar.gz.sha256"
tar -C "$stage" --owner=0 --group=0 --sort=name -czf "$outdir/$name.tar.gz" "$name" || fail "tar"
( cd "$outdir" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
size=$(du -h "$outdir/$name.tar.gz" | cut -f1)
unpacked=$(du -sh "$top" | cut -f1)
echo "PACKAGE OK: $outdir/$name.tar.gz  ($size packed, $unpacked unpacked)"
cat "$outdir/$name.tar.gz.sha256"
echo "highest GLIBC: $maxglibc; GTK player included: ${gtk:-no}"
