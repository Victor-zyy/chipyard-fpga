#!/usr/bin/env python3
"""Focused full-SoC regressions. Host parallelism only; no RTL/test changes.

Install in Chipyard/tests. build-tests compiles ELFs only; run uses existing
simulators. +verbose proves one-shot Skew coverage. Only known Rocket instruction
trace lines are filtered on the HOST; DUT printf/commands remain unchanged.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "generators/gemmini/software/gemmini-rocc-tests/include/gemmini_params_zu19eg.h"
# Long cases first so a short job need not wait for a whole batch to finish.
CASES = {
    "integration": ("gemmini-exact-gather-integration", [],
                    "[PASS] Exact Gather YOLO frontend integration sequence"),
    "silu": ("gemmini-silu-rtl", [], "[PASS] Gemmini RTL SiLU backward compatibility"),
    "exact": ("gemmini-ordering", ["-DORDER_EXACT=1", "-DORDER_ONLY_CASE=0"],
              "[PASS] ordering-v1 early_errors=0 stable_errors=0 total=0"),
    "resadd": ("gemmini-exact-resadd", [],
               "[PASS] Gemmini exact ResAdd: 6 C2f edge pairs, 1536/1536 values exact"),
    # Fresh process: the one-shot delay also targets the fenced control's first A.
    "exact-fenced": ("gemmini-ordering", ["-DORDER_EXACT=1", "-DORDER_ONLY_CASE=2"],
                     "[PASS] ordering-v1 early_errors=0 stable_errors=0 total=0"),
}
CORE_TRACE = re.compile(r"^C\d+:\s+\d+\s+\[[01]\]\s+pc=\[")
EVENT = re.compile(r"\[ORDER-SKEW\] (select|captured|released)\b")
BAD = re.compile(r"\[FAIL\]|\*\*\* FAILED|%Error|%Fatal|Assertion failed|could not open")


def stamp():
    return datetime.now().astimezone().isoformat(timespec="seconds")


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def elf_path(directory, name):
    return directory / ("skew-" + name + ".riscv")


def build(args, directory):
    cc = os.environ.get("SKEW_ELF_CC", str(ROOT / ".conda-env/riscv-tools/bin/riscv64-unknown-elf-gcc"))
    if not Path(cc).is_file():
        cc = shutil.which(cc) or shutil.which("riscv64-unknown-elf-gcc")
    if not cc:
        raise ValueError("RISC-V ELF compiler not found; set SKEW_ELF_CC")
    directory.mkdir(parents=True, exist_ok=True)
    records = []
    for name in CASES:
        source, defines, _ = CASES[name]
        source = ROOT / "tests" / (source + ".c")
        elf = elf_path(directory, name)
        cmd = [cc, "-O2", "-std=gnu99", "-static", "-march=rv64gc", "-mabi=lp64d",
               "-mcmodel=medany", "-fno-fast-math", "-fno-common", "-fno-builtin-printf",
               "-DBAREMETAL=1", "-specs=htif_nano.specs",
               "-I" + str(ROOT / "generators/gemmini/software/gemmini-rocc-tests"),
               "-include", str(HEADER), "-T", str(ROOT / "tests/htif.ld")]
        cmd += defines + [str(source), "-o", str(elf), "-lm"]
        print("[BUILD] " + name, flush=True)
        subprocess.run(cmd, check=True)
        records.append(dict(case=name, command=cmd, elf=str(elf), elf_sha256=sha(elf),
                            source_sha256=sha(source), header_sha256=sha(HEADER)))
    (directory / "build-manifest.json").write_text(json.dumps(records, indent=2) + "\n")
    print("[DONE] ELFs only; no RTL build or simulation: " + str(directory), flush=True)


class Runner:
    def __init__(self, args, directory, sim, logs):
        self.args, self.directory, self.sim, self.logs = args, directory, sim, logs
        self.lock = threading.Lock()
        self.children = set()
        self.stopped = threading.Event()

    def cancel(self):
        self.stopped.set()
        with self.lock:
            processes = list(self.children)
        for proc in processes:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for proc in processes:
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def run_one(self, name):
        if self.stopped.is_set():
            return dict(case=name, exit=130, result="ABORTED", coverage="INCOMPLETE",
                        selected=0, captured=0, released=0, seconds=0, filtered=0)
        folder = self.logs / name
        folder.mkdir()
        elf = elf_path(self.directory, name)
        cmd = [str(self.sim), "+permissive", "+verbose", "+loadmem=" + str(elf),
               "+max-cycles=" + str(self.args.max_cycles),
               "+verilator+seed+" + str(self.args.seed), "+permissive-off", str(elf)]
        if shutil.which("stdbuf"):
            cmd = ["stdbuf", "-oL", "-eL"] + cmd
        (folder / "command.json").write_text(json.dumps(cmd, indent=2) + "\n")
        print("[RUN] " + name + " log=" + str(folder / "run.log"), flush=True)
        started = time.monotonic()
        counts = dict(select=0, captured=0, released=0)
        events, order_cases = [], []
        passed = bad = timeout = delay_ok = exact_info = fenced_info = False
        filtered = 0
        rc = 127
        with (folder / "run.log").open("w", buffering=1) as log:
            log.write("[START] " + stamp() + "\n[COMMAND] " + shlex.join(cmd) + "\n")
            try:
                # Separate cwd and process group per job; no shared mutable output files.
                with self.lock:
                    if self.stopped.is_set():
                        raise InterruptedError("suite interrupted")
                    proc = subprocess.Popen(cmd, cwd=folder, stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, errors="replace", start_new_session=True)
                    self.children.add(proc)
                try:
                    with proc.stdout:
                        for line in proc.stdout:
                            event = EVENT.search(line)
                            if event:
                                label = event.group(1)
                                counts[label] += 1
                                events.append(label)
                                if label == "select":
                                    delay_ok = bool(re.search(r"delay=\s*4096\b", line))
                            passed |= line.strip() == CASES[name][2]
                            bad |= bool(BAD.search(line))
                            timeout |= "(timeout)" in line
                            exact_info |= bool(re.search(r"\[INFO\] DIM=16 ACC_ROWS=1024 exact=1", line))
                            fenced_info |= "[ATTR] ordering-attribution-v1 single_case=2 exact=1" in line
                            case = re.search(r"\[ORDER-CASE\] (\w+) exact=1\b", line)
                            if case:
                                order_cases.append(case.group(1))
                            if not self.args.keep_core_trace and CORE_TRACE.match(line) and not bad:
                                filtered += 1
                            else:
                                log.write(line)
                    rc = proc.wait()
                finally:
                    if proc.poll() is None:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
                    with self.lock:
                        self.children.discard(proc)
            except (OSError, InterruptedError) as error:
                log.write("[HOST-ERROR] " + str(error) + "\n")
            elapsed = time.monotonic() - started
            log.write("[EXIT] status=%s time=%s host_seconds=%.3f filtered_core_lines=%d\n" %
                      (rc, stamp(), elapsed, filtered))
        coverage = "OK"
        if self.args.memory == "skew":
            if events != ["select", "captured", "released"] or not delay_ok:
                coverage = "INVALID"
        elif events:
            coverage = "INVALID"
        if name.startswith("exact"):
            expected = ["FENCED_CONTROL"] if name == "exact-fenced" else ["OVERLAP", "FENCED_CONTROL", "MVOUT_ONLY"]
            if not exact_info or order_cases != expected or (name == "exact-fenced" and not fenced_info):
                coverage = "INVALID"
        result = "ERROR"
        if self.stopped.is_set():
            result = "ABORTED"
        elif timeout:
            result = "TIMEOUT"
        elif bad:
            result = "FAIL"
        elif rc == 0 and passed and coverage == "OK":
            result = "PASS"
        record = dict(case=name, exit=rc, result=result, coverage=coverage,
                      selected=counts["select"], captured=counts["captured"], released=counts["released"],
                      seconds=round(elapsed, 3), filtered=filtered)
        (folder / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        print("[RESULT] %s exit=%s result=%s coverage=%s host_seconds=%.3f" %
              (name, rc, result, coverage, elapsed), flush=True)
        return record


def run(args, directory):
    sim_dir = Path(os.environ.get("SKEW_SIM_DIR", str(ROOT / "sims/verilator"))).resolve()
    config = "GemminiRocketZu19egOrdering" + ("Skew" if args.memory == "skew" else "") + "SimConfig"
    sim = sim_dir / ("simulator-chipyard.harness-" + config)
    names = [name for name in CASES if not args.case or name in args.case]
    paths = [sim] + [elf_path(directory, name) for name in names]
    if any(not p.is_file() for p in paths) or not os.access(sim, os.X_OK):
        raise ValueError("Missing simulator/ELF (or simulator not executable); run build-tests first: " + str(paths))
    logs = Path(tempfile.mkdtemp(prefix="skew-regression-" + args.memory + ".", dir=sim_dir))
    print("[LOGS] " + str(logs), flush=True)
    manifest = dict(version="skew-regression-v1", config=config, jobs=args.jobs, seed=args.seed,
                    max_cycles=args.max_cycles, cases=names, keep_core_trace=args.keep_core_trace,
                    scope="bare-metal full-SoC; NOT Linux/MMU or board validation; one-shot Skew per process",
                    hashes={str(p): sha(p) for p in paths + [HEADER, Path(__file__)]})
    for name in names:
        source = ROOT / "tests" / (CASES[name][0] + ".c")
        manifest["hashes"][str(source)] = sha(source)
    (logs / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if (directory / "build-manifest.json").is_file():
        shutil.copyfile(directory / "build-manifest.json", logs / "build-manifest.json")
    runner = Runner(args, directory, sim, logs)
    all_ok = True
    with (logs / "results.tsv").open("w", buffering=1) as results, ThreadPoolExecutor(max_workers=args.jobs) as pool:
        columns = ["case", "exit", "result", "coverage", "selected", "captured", "released", "seconds", "filtered"]
        results.write("\t".join(columns) + "\n")
        try:
            futures = [pool.submit(runner.run_one, name) for name in names]
            for future in as_completed(futures):
                record = future.result()
                results.write("\t".join(str(record[key]) for key in columns) + "\n")
                all_ok &= record["result"] == "PASS"
        except BaseException:
            runner.cancel()
            raise
    print("[DONE] " + str(logs / "results.tsv"), flush=True)
    return 0 if all_ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("build-tests", help="compile five ELFs only; never build RTL")
    command = sub.add_parser("run", help="run existing simulators in independent processes")
    command.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    command.add_argument("--max-cycles", type=int, required=True, help="global TestDriver budget per process; NOT the watchdog")
    command.add_argument("--seed", type=int, default=1)
    command.add_argument("--memory", choices=["skew", "natural"], default="skew")
    command.add_argument("--case", action="append", choices=list(CASES), help="repeat to select cases; default: all five")
    command.add_argument("--keep-core-trace", action="store_true", help="keep huge Rocket instruction trace; default: filter on host")
    args = parser.parse_args()
    directory = Path(os.environ.get("SKEW_ELF_DIR", str(ROOT / "tests/build-skew-regression"))).resolve()
    if args.action == "build-tests":
        build(args, directory)
        return 0
    if args.max_cycles < 1 or args.seed < 0:
        parser.error("max-cycles must be positive; seed must be nonnegative")
    return run(args, directory)


if __name__ == "__main__":
    def stop(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("[ERROR] " + str(error), flush=True)
        raise SystemExit(2)
    except KeyboardInterrupt:
        print("[ABORTED] suite interrupted; child simulators stopped", flush=True)
        raise SystemExit(130)
