#!/usr/bin/env python3
"""
um790bench — Zero-reboot stability, CPU, Metal->Vulkan translation, and sleep readiness suite
for Minisforum UM790 Pro Hackintosh.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import time

BENCH_DIR = os.path.dirname(os.path.realpath(__file__))
METAL_STRESS = os.path.join(BENCH_DIR, "build", "metal_stress")
RESULTS_DIR = os.path.join(BENCH_DIR, "results")
UM790FAN = "/usr/local/bin/um790fan"


def run_cmd(cmd, env=None, timeout=180):
    p = subprocess.run(cmd, env=env, text=True, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def read_sysctls():
    keys = [
        "debug.phx_hang_kills",
        "debug.nvaccel_surface_gpu_timeouts",
        "debug.nvaccel_iop_fail",
        "debug.nvaccel_iop_ok",
        "debug.nvaccel_iop_gap1",
        "debug.nvaccel_iop_gap2",
        "debug.nvaccel_iop_gap3",
        "debug.nvaccel_iop_gap4",
        "debug.nvaccel_iop_copy_us",
        "debug.nvaccel_iop_copy_us_max",
        "debug.phxfb_vbl_late_us_max",
        "debug.phxfb_dbuf_flips",
        "debug.phxfb_dbuf_wait",
    ]
    out = {}
    rc, stdout, _ = run_cmd(["sysctl"] + keys)
    for line in stdout.splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            try:
                out[k.strip()] = int(v.strip())
            except ValueError:
                pass
    return out


def read_fan_status():
    if not os.path.exists(UM790FAN):
        return None
    rc, stdout, _ = run_cmd([UM790FAN, "status", "--json"])
    if rc != 0:
        return None
    try:
        return json.loads(stdout)
    except Exception:
        return None


def audit_nvmtl_log(path):
    counts = {
        "lines": 0,
        "vk_create_image_err": 0,
        "vk_alloc_refused": 0,
        "soft_vram_gate": 0,
        "failed": 0,
        "refused": 0,
    }
    if not os.path.exists(path):
        return counts
    with open(path, "r", errors="ignore") as f:
        for line in f:
            counts["lines"] += 1
            if "-1000158000" in line:
                counts["vk_create_image_err"] += 1
            if "vkAllocateMemory refused" in line:
                counts["vk_alloc_refused"] += 1
            if "soft VRAM gate" in line or "amd soft-gate" in line:
                counts["soft_vram_gate"] += 1
            if "FAILED" in line:
                counts["failed"] += 1
            if "REFUSED" in line:
                counts["refused"] += 1
    return counts


def parse_env_pairs(spec):
    pairs = {}
    if not spec:
        return pairs
    for tok in spec.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            pairs[k] = v
    return pairs


def extract_json_line(stdout):
    for line in reversed(stdout.splitlines()):
        s = line.strip()
        if s.startswith("{") and s.endswith("}"):
            return json.loads(s)
    raise ValueError(f"No JSON line in output:\n{stdout}")


def cmd_run(args):
    os.makedirs(RESULTS_DIR, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    frames = 60 if args.quick else 240
    iters = 14 if args.quick else 30
    cpu_sec = 1.0 if args.quick else 3.0
    vram_mb = 320

    pre_sys = read_sysctls()
    pre_fan = read_fan_status()
    crash_dir = os.path.expanduser("~/Library/Logs/DiagnosticReports")
    start_time = time.time()

    with tempfile.NamedTemporaryFile(prefix="um790bench-nvmtl-", suffix=".log", delete=False) as tf:
        log_path = tf.name

    try:
        env = os.environ.copy()
        env["NVMTL_LOG_PATH"] = log_path
        rc, stdout, stderr = run_cmd([
            METAL_STRESS,
            "--mode", "all",
            "--frames", str(frames),
            "--mb", str(vram_mb),
            "--iters", str(iters),
            "--seconds", str(cpu_sec),
            "--json",
        ], env=env, timeout=240)
        post_sys = read_sysctls()
        post_fan = read_fan_status()
        log_audit = audit_nvmtl_log(log_path)
    finally:
        try:
            os.remove(log_path)
        except OSError:
            pass

    if rc != 0:
        print(f"FAIL: metal_stress exited {rc}\nstderr:\n{stderr}\nstdout:\n{stdout}", file=sys.stderr)
        return 1

    bench = extract_json_line(stdout)

    new_crashes = []
    if os.path.isdir(crash_dir):
        for fn in os.listdir(crash_dir):
            fp = os.path.join(crash_dir, fn)
            try:
                if os.path.getmtime(fp) >= start_time and fn.endswith(".ips"):
                    new_crashes.append(fn)
            except OSError:
                pass

    delta_sys = {}
    for k in pre_sys:
        if k in post_sys:
            delta_sys[k] = post_sys[k] - pre_sys[k]

    report = {
        "timestamp": ts,
        "tag": args.tag or ("quick" if args.quick else "full"),
        "bench": bench,
        "log_audit": log_audit,
        "sysctl_delta": delta_sys,
        "fan_pre": pre_fan,
        "fan_post": post_fan,
        "new_crashes": new_crashes,
    }

    out_file = os.path.join(RESULTS_DIR, f"{ts}-{report['tag']}.json")
    with open(out_file, "w") as f:
        json.dump(report, f, indent=2)

    cpu = bench.get("cpu", {})
    comp = bench.get("composite", {})
    vram = bench.get("vram_stress", {})

    print(f"=== um790bench ({report['tag']}) ===")
    print(f"Device    : {bench.get('device')}")
    if cpu:
        print(f"CPU       : 1T={cpu['score_1t']:.1f} | 8T={cpu['score_8t']:.1f} | 16T={cpu['score_16t']:.1f} Mops/s "
              f"(SMT {cpu['smt_scaling']:.2f}x, throttle={cpu['throttle_ratio_16t']:.3f}, memcpy={cpu['mem_copy_gbs' if 'mem_copy_gbs' in cpu else 'memcpy_gbs']:.2f} GB/s)")
    if comp:
        print(f"Composite : avg={comp['avg_fps']:.1f} FPS (<8.33ms: {comp['pct_under_833ms']:.1f}%) | "
              f"enc P50/P95/P99={comp['encode_p50_ms']:.3f}/{comp['encode_p95_ms']:.3f}/{comp['encode_p99_ms']:.3f} ms | "
              f"tot P50/P95/P99={comp['total_p50_ms']:.3f}/{comp['total_p95_ms']:.3f}/{comp['total_p99_ms']:.3f} ms | "
              f"CPU={comp['cpu_pct']:.1f}%")
    if vram:
        print(f"VRAM/Pipe : ok={vram['ok']} | held={vram['textures_held']} ({vram['target_mb']} MiB) | "
              f"pipelines={vram['pipelines_compiled']} | wall={vram['wall_ms']:.1f} ms")
    print(f"NVMTL Log : lines={log_audit['lines']} | -1000158000={log_audit['vk_create_image_err']} | "
          f"alloc_refused={log_audit['vk_alloc_refused']} | FAILED={log_audit['failed']} | REFUSED={log_audit['refused']}")
    if pre_fan and post_fan:
        c0 = pre_fan["fans"][0]["temp_c"]
        c1 = post_fan["fans"][0]["temp_c"]
        r1 = post_fan["fans"][0]["rpm"]
        print(f"Thermals  : CPU {c0}C -> {c1}C ({r1} RPM)")
    print(f"Sysctl Δ  : hang_kills={delta_sys.get('debug.phx_hang_kills', 0)} | "
          f"gpu_timeouts={delta_sys.get('debug.nvaccel_surface_gpu_timeouts', 0)} | "
          f"iop_fail={delta_sys.get('debug.nvaccel_iop_fail', 0)} | "
          f"new_crashes={len(new_crashes)}")
    print(f"Saved     : {out_file}")
    return 0


def cmd_ab(args):
    env_a = parse_env_pairs(args.envA)
    env_b = parse_env_pairs(args.envB)
    mode = args.mode
    frames = args.frames

    def run_one(label, extra_env):
        with tempfile.NamedTemporaryFile(prefix=f"um790ab-{label}-", suffix=".log", delete=False) as tf:
            log_path = tf.name
        try:
            env = os.environ.copy()
            env.update(extra_env)
            env["NVMTL_LOG_PATH"] = log_path
            rc, stdout, stderr = run_cmd([
                METAL_STRESS, "--mode", mode, "--frames", str(frames), "--json"
            ], env=env, timeout=180)
            audit = audit_nvmtl_log(log_path)
        finally:
            try:
                os.remove(log_path)
            except OSError:
                pass
        if rc != 0:
            raise RuntimeError(f"variant {label} failed (rc={rc}): {stderr}")
        return extract_json_line(stdout), audit

    res_a, aud_a = run_one("A", env_a)
    res_b, aud_b = run_one("B", env_b)

    print(f"=== A/B Comparison (mode={mode}, frames={frames}) ===")
    print(f"  A: {args.envA or '(default)'}")
    print(f"  B: {args.envB or '(default)'}")
    if mode == "composite":
        ca, cb = res_a["composite"], res_b["composite"]
        def row(name, va, vb, unit="", lower_better=True):
            diff = ((vb - va) / va * 100.0) if va else 0.0
            tag = ""
            if va and vb != va:
                better = (vb < va) if lower_better else (vb > va)
                tag = f"({'faster' if better else 'slower'} {abs(diff):.1f}%)"
            print(f"  {name:<18} A={va:>8.3f}{unit}   B={vb:>8.3f}{unit}   {tag}")
        row("Encode P50", ca["encode_p50_ms"], cb["encode_p50_ms"], "ms")
        row("Encode P95", ca["encode_p95_ms"], cb["encode_p95_ms"], "ms")
        row("Total P50",  ca["total_p50_ms"],  cb["total_p50_ms"],  "ms")
        row("Total P95",  ca["total_p95_ms"],  cb["total_p95_ms"],  "ms")
        row("Avg FPS",    ca["avg_fps"],       cb["avg_fps"],       "", lower_better=False)
        row("Process CPU", ca["cpu_pct"],      cb["cpu_pct"],       "%")
    else:
        va, vb = res_a["vram_stress"], res_b["vram_stress"]
        print(f"  Wall ms : A={va['wall_ms']:.1f} ms | B={vb['wall_ms']:.1f} ms")
    print(f"  NVMTL log lines : A={aud_a['lines']} (-1000158000={aud_a['vk_create_image_err']}) | "
          f"B={aud_b['lines']} (-1000158000={aud_b['vk_create_image_err']})")
    return 0


def cmd_audit(args):
    print("=== 1. Kernel Drivers & Display Health ===")
    _, kext_out, _ = run_cmd(["kextstat"])
    for pat in ["PhxBringup", "PhxAccel", "UM790Fan", "VirtualSMC", "Lilu", "RestrictEvents"]:
        hits = [ln.strip() for ln in kext_out.splitlines() if pat in ln]
        print(f"  {pat:<14}: {hits[0] if hits else 'NOT LOADED'}")

    sys_vals = read_sysctls()
    g1 = sys_vals.get("debug.nvaccel_iop_gap1", 0)
    g2 = sys_vals.get("debug.nvaccel_iop_gap2", 0)
    g3 = sys_vals.get("debug.nvaccel_iop_gap3", 0)
    g4 = sys_vals.get("debug.nvaccel_iop_gap4", 0)
    tot_gaps = g1 + g2 + g3 + g4
    drop_pct = ((g2 + g3) * 100.0 / tot_gaps) if tot_gaps else 0.0
    print(f"  Hang kills    : {sys_vals.get('debug.phx_hang_kills', '?')} | "
          f"GPU timeouts: {sys_vals.get('debug.nvaccel_surface_gpu_timeouts', '?')} | "
          f"iop_fail: {sys_vals.get('debug.nvaccel_iop_fail', '?')}")
    print(f"  Frame gaps    : 1x={g1} 2x={g2} 3x={g3} 4x+={g4} (2-3x skip ratio={drop_pct:.2f}%)")
    print(f"  Copy us / max : {sys_vals.get('debug.nvaccel_iop_copy_us', '?')} / {sys_vals.get('debug.nvaccel_iop_copy_us_max', '?')} us | "
          f"VBL late max={sys_vals.get('debug.phxfb_vbl_late_us_max', '?')} us")

    _, ws_out, _ = run_cmd(["ps", "-axo", "pid,pcpu,rss,comm"])
    for ln in ws_out.splitlines():
        if "WindowServer" in ln or "phxpowerd" in ln or "UM790FanBar" in ln:
            print(f"  proc: {ln.strip()}")

    print("\n=== 2. Sleep & Power Management Audit (Read-Only) ===")
    _, pm_g, _ = run_cmd(["pmset", "-g"])
    for ln in pm_g.splitlines():
        if re.search(r"SleepDisabled|sleep\s|displaysleep|hibernatemode|standby\b|powernap|womp", ln):
            print(f"  {ln.strip()}")
    _, pm_ass, _ = run_cmd(["pmset", "-g", "assertions"])
    in_list = False
    for ln in pm_ass.splitlines():
        if "Listed by owning process:" in ln:
            in_list = True
        if in_list:
            print(f"  {ln}")
        if "Kernel Assertions:" in ln:
            break

    print("\n=== 3. Recent Chrome GPU Crashes ===")
    crash_dir = os.path.expanduser("~/Library/Logs/DiagnosticReports")
    if os.path.isdir(crash_dir):
        ips = sorted([f for f in os.listdir(crash_dir) if "Google Chrome" in f and f.endswith(".ips")])
        print(f"  Chrome crash reports: {len(ips)} (latest: {ips[-1] if ips else 'none'})")

    if args.check_auxkc:
        print("\n=== 4. AuxKC Dry-Run Link Check ===")
        dry = "/tmp/um790-aux-dryrun.kc"
        try:
            if os.path.exists(dry):
                os.remove(dry)
            cmd = [
                "sudo", "kmutil", "create", "-n", "aux",
                "--volume-root", "/", "--allow-missing-kdk",
                "--kernel", "/System/Library/Kernels/kernel",
                "-B", "/System/Library/KernelCollections/BootKernelExtensions.kc",
                "-S", "/System/Library/KernelCollections/SystemKernelExtensions.kc",
                "--repository", "/Library/Extensions",
                "--repository", "/Library/Apple/System/Library/Extensions",
                "-A", dry, "-z"
            ]
            for b in ["/Library/Extensions/PhxAccel.kext", "/Library/Extensions/UM790Fan.kext"]:
                if os.path.isdir(b):
                    cmd.extend(["--bundle-path", b])
            rc, out, err = run_cmd(cmd, timeout=120)
            if rc != 0:
                print(f"  AuxKC dry-run FAILED (rc={rc}): {err}")
            else:
                rc2, ins, _ = run_cmd(["kmutil", "inspect", "-a", "x86_64", "-A", dry])
                print("  AuxKC dry-run OK:")
                for ln in ins.splitlines():
                    if any(x in ln for x in ["PhxAccel", "UM790Fan", "HighPoint", "AppleMobileDevice"]):
                        print(f"    {ln}")
        finally:
            if os.path.exists(dry):
                os.remove(dry)
    return 0


def cmd_compare(args):
    with open(args.fileA) as f:
        a = json.load(f)
    with open(args.fileB) as f:
        b = json.load(f)
    print(f"=== Compare {os.path.basename(args.fileA)} vs {os.path.basename(args.fileB)} ===")
    for sec, keys in [
        ("cpu", ["score_1t", "score_8t", "score_16t", "memcpy_gbs"]),
        ("composite", ["encode_p50_ms", "encode_p95_ms", "total_p50_ms", "total_p95_ms", "avg_fps", "cpu_pct"]),
        ("vram_stress", ["wall_ms"]),
    ]:
        sa = a.get("bench", {}).get(sec, {})
        sb = b.get("bench", {}).get(sec, {})
        for k in keys:
            if k in sa and k in sb:
                va, vb = sa[k], sb[k]
                pct = ((vb - va) / va * 100.0) if va else 0.0
                print(f"  {sec}.{k:<20}: {va:>8.2f} -> {vb:>8.2f} ({pct:+.1f}%)")
    return 0


def main():
    p = argparse.ArgumentParser(prog="um790bench", description="UM790 Pro Hackintosh stability & performance suite")
    sub = p.add_subparsers(dest="cmd", required=True)

    pr = sub.add_parser("run", help="Run CPU, Metal composite, and VRAM/pipeline stress benchmark")
    pr.add_argument("--quick", action="store_true", help="Shorter quick run (~5s)")
    pr.add_argument("--full", action="store_true", help="Full run (~15s)")
    pr.add_argument("--tag", default="", help="Optional label for saved JSON report")
    pr.set_defaults(func=cmd_run)

    pa = sub.add_parser("ab", help="Run isolated A/B comparison with different NVMTL environment variables")
    pa.add_argument("--envA", default="", help='Space-separated KEY=VAL for run A')
    pa.add_argument("--envB", default="", help='Space-separated KEY=VAL for run B')
    pa.add_argument("--mode", choices=["composite", "vram-stress"], default="composite")
    pa.add_argument("--frames", type=int, default=120)
    pa.set_defaults(func=cmd_ab)

    pau = sub.add_parser("audit", help="Read-only stability, display, and sleep readiness audit")
    pau.add_argument("--check-auxkc", action="store_true", help="Also run non-destructive AuxKC link dry-run")
    pau.set_defaults(func=cmd_audit)

    pc = sub.add_parser("compare", help="Compare two saved JSON benchmark reports")
    pc.add_argument("fileA")
    pc.add_argument("fileB")
    pc.set_defaults(func=cmd_compare)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
