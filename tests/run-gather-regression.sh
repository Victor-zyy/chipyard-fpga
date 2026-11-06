#!/usr/bin/env bash
# Build only, or run ONE existing full-SoC simulator. Never builds RTL/FPGA.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
elf_dir="${GATHER_ELF_DIR:-$root/tests/build-gather-regression}"
sim_dir="${GATHER_SIM_DIR:-$root/sims/verilator}"
header="$root/generators/gemmini/software/gemmini-rocc-tests/include/gemmini_params_zu19eg.h"

case "${1:-help}" in
help|--help|-h)
    printf '%s\n' \
      'Usage: bash tests/run-gather-regression.sh build-tests' \
      '       GATHER_MAX_CYCLES=N bash tests/run-gather-regression.sh run short-diag|short|full-diag|full natural|skew' \
      'short: 2 rounds each of Gather and WS/Gather; full: 19 rounds each.' \
      'All variants keep full matrix dimensions, reference arithmetic and elementwise checks.' \
      '*-diag adds prepare/check timestamps. Its PASS is diagnostic, not timing-neutral acceptance.' \
      'Run the matching non-diag variant for acceptance; the original test prints remain.' \
      'Budget is explicit: total simulator cycles include boot, CPU reference/check and output.' \
      'TIMEOUT is incomplete, not a data mismatch. No watchdog change, fence or delay workaround.' \
      'Environment: GATHER_ELF_CC, GATHER_ELF_DIR, GATHER_SIM_DIR, GATHER_MAX_CYCLES, GATHER_SEED=1.'
    ;;
build-tests)
    cc="${GATHER_ELF_CC:-$root/.conda-env/riscv-tools/bin/riscv64-unknown-elf-gcc}"
    if [[ ! -x "$cc" ]]; then cc=$(command -v riscv64-unknown-elf-gcc); fi
    mkdir -p "$elf_dir"
    for variant in short-diag short full-diag full; do
        repeats=19
        diag=0
        if [[ "$variant" == short* ]]; then repeats=2; fi
        if [[ "$variant" == *-diag ]]; then diag=1; fi
        elf="$elf_dir/gemmini-exact-gather-$variant.riscv"
        echo "[BUILD] $variant repeats=$repeats phase_diag=$diag"
        "$cc" -O2 -std=gnu99 -static -march=rv64gc -mabi=lp64d -mcmodel=medany \
          -fno-fast-math -fno-common -fno-builtin-printf -DBAREMETAL=1 \
          -specs=htif_nano.specs \
          -I"$root/generators/gemmini/software/gemmini-rocc-tests" -include "$header" \
          -T "$root/tests/htif.ld" \
          -DYOLO_STRESS_REPEATS="$repeats" -DGEMMINI_GATHER_PHASE_DIAG="$diag" \
          "$root/tests/gemmini-exact-gather.c" -o "$elf" -lm
        sha256sum "$elf"
    done
    ;;
run)
    variant="${2:-short-diag}"
    memory="${3:-natural}"
    case "$variant" in short|short-diag|full|full-diag) ;;
        *) echo "Invalid variant: $variant" >&2; exit 2;; esac
    case "$memory" in
        natural) cfg=GemminiRocketZu19egOrderingSimConfig ;;
        skew) cfg=GemminiRocketZu19egOrderingSkewSimConfig ;;
        *) echo "Invalid memory mode: $memory" >&2; exit 2 ;;
    esac
    max_cycles="${GATHER_MAX_CYCLES:-}"
    seed="${GATHER_SEED:-1}"
    if [[ ! "$max_cycles" =~ ^[1-9][0-9]*$ || ! "$seed" =~ ^[0-9]+$ ]]; then
        echo 'Set a positive GATHER_MAX_CYCLES explicitly; budget is not the Gemmini watchdog.' >&2
        exit 2
    fi
    repeats=19
    diag=0
    if [[ "$variant" == short* ]]; then repeats=2; fi
    if [[ "$variant" == *-diag ]]; then diag=1; fi
    expected_values=$((4601 + repeats * 81920))
    elf="$elf_dir/gemmini-exact-gather-$variant.riscv"
    sim="$sim_dir/simulator-chipyard.harness-$cfg"
    [[ -f "$elf" && -x "$sim" ]] || { echo "Missing ELF or simulator: $elf / $sim" >&2; exit 2; }
    # Resolve relative overrides before changing working directory.
    elf=$(readlink -f "$elf")
    sim=$(readlink -f "$sim")
    log_dir=$(mktemp -d "$sim_dir/gather-$variant-$memory.XXXXXX")
    log_dir=$(cd "$log_dir" && pwd)
    log="$log_dir/run.log"
    echo "[LOGS] $log_dir"
    {
        echo 'gather-regression-v1; bare-metal full-SoC, not Linux/MMU or benchmark coverage'
        printf 'variant=%s config=%s repeats=%s phase_diag=%s max_cycles=%s seed=%s\n' \
          "$variant" "$cfg" "$repeats" "$diag" "$max_cycles" "$seed"
        sha256sum "$root/tests/gemmini-exact-gather.c" "$header" "$elf" "$sim"
    } > "$log_dir/manifest.txt"
    printf 'config\tvariant\texit\tresult\tcoverage\n' > "$log_dir/results.tsv"
    prefix=()
    if command -v stdbuf >/dev/null 2>&1; then prefix=(stdbuf -oL -eL); fi
    cd "$sim_dir"
    echo "[RUN] $cfg $variant budget=$max_cycles"
    printf '[START] %s\n' "$(date -Is)" > "$log"
    rc=0
    # Deliberately omit +verbose: no huge cycle-by-cycle core trace is needed.
    # stdout and stderr go straight to the log (no tee/SIGPIPE on terminal loss).
    "${prefix[@]}" "$sim" +permissive +loadmem="$elf" +max-cycles="$max_cycles" \
      +verilator+seed+"$seed" +permissive-off "$elf" >> "$log" 2>&1 || rc=$?
    printf '[EXIT] status=%s time=%s\n' "$rc" "$(date -Is)" >> "$log"
    grep -E '\[INFO\]|\[GATHER-DIAG\]|\[STAGE\]|\[STRESS-|\[INTERLEAVE-|\[(PASS|FAIL|EXIT)\]|%Error|%Fatal|timeout|mismatch|could not open' \
      "$log" > "$log_dir/summary.txt" || true
    result=ERROR
    coverage=INCOMPLETE
    if grep -Fq '(timeout)' "$log"; then
        result=TIMEOUT
    elif grep -Fq '[FAIL]' "$log"; then
        result=FAIL
    elif [[ "$rc" == 0 ]] && \
         grep -Fq "[PASS] Gemmini native RTL Exact Gather: $expected_values/$expected_values values exact" "$log" && \
         grep -Fxq "[PASS] YOLO gather-only stress repeats=$repeats" "$log" && \
         grep -Fxq "[PASS] YOLO WS/Gather interleave repeats=$repeats" "$log"; then
        result=PASS
        coverage=OK
        if [[ "$diag" == 1 ]] && ! grep -Fq "[INFO] gather-progress-v1 MAIN_ENTER repeats=$repeats phase_diag=1" "$log"; then
            result=ERROR
            coverage=INVALID
        fi
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$cfg" "$variant" "$rc" "$result" "$coverage" >> "$log_dir/results.tsv"
    echo "[RESULT] $cfg $variant status=$rc result=$result coverage=$coverage"
    echo "[DONE] $log_dir/results.tsv"
    if [[ "$result" != PASS ]]; then exit 1; fi
    ;;
*) echo 'Use --help for usage' >&2; exit 2 ;;
esac
