#!/usr/bin/env bash
# Simulation-only configuration ablation. No FPGA, Linux or performance claim.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sim_dir="$root/sims/verilator"
elf_dir="${ATTR_ELF_DIR:-$root/tests/build-ordering-attribution}"
group="${2:-all}"
configs=()
case "$group" in all|pipe|legacy|noext) ;; *) echo "Unknown group: $group" >&2; exit 2;; esac
for variant in pipe legacy noext; do
    if [[ "$group" != all && "$group" != "$variant" ]]; then continue; fi
    case "$variant" in
        pipe) stem=GemminiRocketZu19egOrdering ;;
        legacy) stem=GemminiRocketZu19egOrderingLegacy ;;
        noext) stem=GemminiRocketZu19egOrderingNoExt ;;
    esac
    configs+=("${stem}SimConfig" "${stem}SkewSimConfig")
done
case "${1:-help}" in
help|--help|-h)
    printf '%s\n' \
      'Usage: bash tests/run-ordering-attribution.sh build-tests|build-sims|run [all|pipe|legacy|noext]' \
      'Run from an initialized Chipyard toolchain environment.' \
      'build-tests: compile two RAW ELFs; no simulation.' \
      'build-sims : build selected natural/skew simulators sequentially; no simulation.' \
      'run        : same RAW ELFs, seed and 4096-cycle skew across selected configs.' \
      '             overlap/fenced each start a fresh simulator process.' \
      'pipe       : current ScalePipe + extensions + busy fix.' \
      'legacy     : legacy scalers (MVIN latency 4); extensions + busy fix retained.' \
      'noext      : legacy + SiLU/ExactResAdd/ExactGather/Layer/DSP mapping disabled.' \
      'IMPORTANT: noext uses current shared RTL, NOT an unmodified upstream checkout.' \
      'A RAW failure does not exercise Exact arithmetic; do not claim numeric failure.' \
      'No dependency fix, software-delay workaround, watchdog change, or board config change.' \
      'Environment: BUILD_JOBS=8, ATTR_MAX_CYCLES=20000000, ATTR_SEED=1, ATTR_ELF_DIR.'
    ;;
build-tests)
    cc="${ORDERING_ELF_CC:-$root/.conda-env/riscv-tools/bin/riscv64-unknown-elf-gcc}"
    if [[ ! -x "$cc" ]]; then cc=$(command -v riscv64-unknown-elf-gcc); fi
    header="$root/generators/gemmini/software/gemmini-rocc-tests/include/gemmini_params_zu19eg.h"
    mkdir -p "$elf_dir"
    for test_case in overlap fenced; do
        case_id=1
        if [[ "$test_case" == fenced ]]; then case_id=2; fi
        "$cc" -O2 -std=gnu99 -static -march=rv64gc -mabi=lp64d -mcmodel=medany \
          -fno-fast-math -fno-common -DBAREMETAL=1 -specs=htif_nano.specs \
          -I"$root/generators/gemmini/software/gemmini-rocc-tests" -include "$header" \
          -T "$root/tests/htif.ld" -DORDER_EXACT=0 -DORDER_ONLY_CASE="$case_id" \
          "$root/tests/gemmini-ordering.c" -o "$elf_dir/gemmini-ordering-raw-$test_case.riscv" -lm
    done
    sha256sum "$elf_dir"/gemmini-ordering-raw-{overlap,fenced}.riscv
    ;;
build-sims)
    for cfg in "${configs[@]}"; do
        echo "[BUILD] $cfg"
        make -C "$sim_dir" -j"${BUILD_JOBS:-8}" \
          SBT_PROJECT=chipyard_fpga MODEL=TestHarness MODEL_PACKAGE=chipyard.harness \
          CONFIG="$cfg" CONFIG_PACKAGE=chipyard.fpga.zu19eg GENERATOR_PACKAGE=chipyard \
          TB=TestDriver TOP=ChipTop VERILATOR_THREADS=1
    done
    ;;
run)
    max_cycles="${ATTR_MAX_CYCLES:-20000000}"
    seed="${ATTR_SEED:-1}"
    [[ "$max_cycles" =~ ^[1-9][0-9]*$ && "$seed" =~ ^[0-9]+$ ]] || { echo 'Invalid cycle budget/seed' >&2; exit 2; }
    # Check the whole matrix before starting; never report missing builds as RTL failures.
    for cfg in "${configs[@]}"; do
        [[ -x "$sim_dir/simulator-chipyard.harness-$cfg" ]] || { echo "Missing simulator: $cfg" >&2; exit 2; }
    done
    for test_case in overlap fenced; do
        [[ -f "$elf_dir/gemmini-ordering-raw-$test_case.riscv" ]] || { echo 'Run build-tests first' >&2; exit 2; }
    done
    log_dir=$(mktemp -d "$sim_dir/ordering-attribution.XXXXXX")
    echo "[LOGS] $log_dir"
    {
        echo 'ordering-attribution-v1; configuration ablation, NOT pristine upstream'
        printf 'group=%s seed=%s max_cycles=%s\n' "$group" "$seed" "$max_cycles"
        git -C "$root" rev-parse HEAD
        git -C "$root/generators/gemmini" rev-parse HEAD
        sha256sum "$root/tests/gemmini-ordering.c" \
          "$root/fpga/src/main/scala/zu19eg/OrderingConfigs.scala" \
          "$root/fpga/src/main/scala/zu19eg/OrderingAttributionConfigs.scala" \
          "$root/generators/gemmini/src/main/scala/gemmini/OrderingDiagnostics.scala" \
          "$elf_dir"/gemmini-ordering-raw-{overlap,fenced}.riscv
        for cfg in "${configs[@]}"; do sha256sum "$sim_dir/simulator-chipyard.harness-$cfg"; done
    } > "$log_dir/manifest.txt"
    printf 'config\tcase\texit\tresult\tselected\tcaptured\treleased\tcoverage\n' > "$log_dir/results.tsv"
    all_ok=1
    cd "$sim_dir"
    for cfg in "${configs[@]}"; do
        for test_case in overlap fenced; do
            elf="$elf_dir/gemmini-ordering-raw-$test_case.riscv"
            log="$log_dir/$cfg-$test_case.log"
            echo "[RUN] $cfg $test_case"
            rc=0
            "./simulator-chipyard.harness-$cfg" \
              +permissive +verbose +loadmem="$elf" +max-cycles="$max_cycles" \
              +verilator+seed+"$seed" +permissive-off "$elf" > "$log" 2>&1 || rc=$?
            grep -E '\[INFO\]|\[ATTR\]|\[ORDER-(CASE|CHECK|DIFF|GUARD|ALERT|SKEW)\]|\[(PASS|FAIL)\]|FAILED|PASSED|%Error|%Fatal|timeout|could not open' \
              "$log" > "${log%.log}-summary.txt" || true
            result=ERROR
            if grep -q '\[FAIL\] ordering-v1' "$log"; then result=FAIL; fi
            if [[ "$rc" == 0 ]] && grep -q '\[PASS\] ordering-v1 early_errors=0 stable_errors=0 total=0' "$log"; then result=PASS; fi
            selected=$(grep -c '\[ORDER-SKEW\] select ' "$log" || true)
            captured=$(grep -c '\[ORDER-SKEW\] captured ' "$log" || true)
            released=$(grep -c '\[ORDER-SKEW\] released ' "$log" || true)
            coverage=OK
            expected_id=1
            expected_case=OVERLAP
            if [[ "$test_case" == fenced ]]; then expected_id=2; expected_case=FENCED_CONTROL; fi
            if ! grep -q "\[ATTR\] ordering-attribution-v1 single_case=$expected_id exact=0" "$log" || \
               [[ $(grep -c '\[ORDER-CASE\]' "$log" || true) != 1 ]] || \
               ! grep -q "\[ORDER-CASE\] $expected_case " "$log"; then coverage=INVALID; fi
            if [[ "$cfg" == *Skew* ]]; then
                if [[ "$selected/$captured/$released" != 1/1/1 ]]; then coverage=INVALID; fi
            elif [[ "$selected/$captured/$released" != 0/0/0 ]]; then coverage=INVALID; fi
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
              "$cfg" "$test_case" "$rc" "$result" "$selected" "$captured" "$released" "$coverage" >> "$log_dir/results.tsv"
            echo "[RESULT] $cfg $test_case status=$rc result=$result coverage=$coverage"
            if [[ "$result" != PASS || "$coverage" != OK ]]; then all_ok=0; fi
        done
    done
    echo "[DONE] $log_dir/results.tsv (FAIL is evidence, not an automatic attribution to upstream)"
    if [[ "$all_ok" != 1 ]]; then exit 1; fi
    ;;
*) echo 'Use --help for usage' >&2; exit 2 ;;
esac
