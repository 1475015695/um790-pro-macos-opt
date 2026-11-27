#!/usr/bin/env python3
"""
um790watch — Lightweight resident background monitor for UM790 Pro Hackintosh.

Passively audits PhxAccel / PhxBringup / NVMTLDriver / RADV health, crash reports,
120Hz frame pacing, copy/VBL latencies, WindowServer/CPU/memory pressure, and EC thermals.
Uses zero Metal/GPU calls; unloading stops 100% of background work.
"""

import argparse
from collections import Counter, deque
import copy
import datetime
import json
import os
import re
import signal
import subprocess
import sys
import time

SCRIPT_PATH = os.path.realpath(__file__)
SUPPORT_DIR = os.path.expanduser("~/Library/Application Support/UM790Watch")
CONFIG_PATH = os.path.join(SUPPORT_DIR, "config.json")
LOG_DIR = os.path.expanduser("~/Library/Logs/um790-watch")
EVENTS_JSONL = os.path.join(LOG_DIR, "events.jsonl")
EVENTS_LOG = os.path.join(LOG_DIR, "events.log")
LATEST_REPORT_MD = os.path.join(LOG_DIR, "latest-report.md")
DAEMON_LOG = os.path.join(LOG_DIR, "daemon.log")
STATUS_PATH = "/tmp/um790watch-status.json"
LAUNCHD_LABEL = "local.um790watch"
LAUNCHD_PLIST = os.path.expanduser("~/Library/LaunchAgents/%s.plist" % LAUNCHD_LABEL)
PLIST_TEMPLATE = os.path.join(os.path.dirname(SCRIPT_PATH), "local.um790watch.plist.template")
UM790FAN = "/usr/local/bin/um790fan"
CRASH_DIRS = [
    "/Library/Logs/DiagnosticReports",
    os.path.expanduser("~/Library/Logs/DiagnosticReports"),
]

DEFAULT_CONFIG = {
    "enabled": True,
    "notify": True,
    "notify_min_severity": "critical",
    "interval_sec": 5,
    "log_max_bytes": 5 * 1024 * 1024,
    "cooldown_sec": 60,
    "ack_ts": 0.0,
    "thresholds": {
        "frame_drop_pct_warn": 18.0,
        "frame_drop_min_frames": 40,
        "gap4_burst_warn": 6,
        "iop_copy_avg_us_warn": 2000,
        "iop_copy_max_us_warn": 4500,
        "vbl_late_avg_us_warn": 1000,
        "vbl_late_max_us_warn": 10000,
        "gpu_wait_max_us_warn": 25000,
        "ws_cpu_pct_warn": 95.0,
        "ws_rss_mb_warn": 1536,
        "proc_cpu_pct_warn": 300.0,
        "load1_warn": 12.0,
        "cpu_temp_c_warn": 88,
        "reclaim_burst_warn": 8,
        "host_ptr_refused_burst_warn": 20,
    },
}

SYSCTL_KEYS = [
    "debug.phx_hang_kills",
    "debug.nvaccel_surface_gpu_timeouts",
    "debug.nvaccel_surface_gpu_wait_max_us",
    "debug.nvaccel_iop_fail",
    "debug.nvaccel_iop_ok",
    "debug.nvaccel_iop_gap1",
    "debug.nvaccel_iop_gap2",
    "debug.nvaccel_iop_gap3",
    "debug.nvaccel_iop_gap4",
    "debug.nvaccel_iop_copy_us",
    "debug.nvaccel_iop_copy_us_max",
    "debug.nvaccel_iop_copy_us_sum",
    "debug.nvaccel_iop_async_refused",
    "debug.nvaccel_iop_async_drop",
    "debug.nvaccel_iop_late",
    "debug.nvaccel_vm_refused",
    "debug.phxfb_vbl_n",
    "debug.phxfb_vbl_late_us_sum",
    "debug.phxfb_vbl_late_us_max",
    "debug.phxfb_hdmi_reset",
    "kern.memorystatus_vm_pressure_level",
    "vm.loadavg",
]

NVMTL_LINE_RE = re.compile(r"^pid\s+(\d+)\s+NVMTL:\s*(.*)$")
LOADAVG_RE = re.compile(r"\{\s*([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s*\}")

GPU_PROCS = ("WindowServer", "SecurityAgent", "loginwindow", "Google Chrome Helper")
GPU_FRAME_MARKERS = ("NVMTLDriver", "libvulkan_radeon", "libvulkan", "Phx", "Metal", "QuartzCore")
CRASH_EXTS = (".ips", ".diag", ".hang")

REMEDIATION_HINTS = {
    "GPU_HANG_KILL": "触发了内核 GPU hang 超时保护，建议检查 ~/Library/Logs/um790-watch/events.jsonl 与最近崩溃的 .ips，确认是否为特定应用/管线死锁。",
    "SURFACE_GPU_TIMEOUT": "IOMFB/Surface GPU 等待超时 (>100ms)，说明 GPU 队列出现明显阻塞或显存争用。",
    "SURFACE_GPU_WAIT_SPIKE": "Surface GPU 最大等待峰值升高，表明瞬时渲染提交或换页压力较大。",
    "IOP_DISPLAY_FAIL": "显示输出或 VM 映射计数异常，检查外接显示器线材、PhxFB/PhxAccel 状态或显存紧张情况。",
    "IOP_ASYNC_DROP": "异步显示拷贝出现丢帧或被拒，可能高刷新率下 CPU/内存带宽瞬时饱和。",
    "PROCESS_CRASH_GPU": "发生与图形驱动/Metal/RADV 相关的进程崩溃，查看 DiagnosticReports 对应 .ips 堆栈定位模块与偏移。",
    "RADV_OOM": "RADV 设备内存不足或丢失 (VK_ERROR_OUT_OF_DEVICE_MEMORY / DEVICE_LOST)，建议关闭多余高显存标签页并运行 um790bench audit。",
    "NVMTL_DRIVER_ERROR": "NVMTLDriver 报告渲染/创建失败，结合对应进程与日志关键字排查。",
    "NVMTL_VRAM_PRESSURE": "NVMTLDriver 频繁触发软显存门槛或缓存紧急回收，存在显存/GTT 争用瓶颈。",
    "NVMTL_HOST_PTR_BURST": "大量宿主机指针导入被 Vulkan 拒绝（可能涉及只读页或内存对齐），若伴随卡顿需核查对应进程。",
    "FRAME_DROP_BURST": "持续出现多帧跳过 (2x/3x/4x+)，检查当前高 CPU 进程、WindowServer 负载及显示链路瓶颈。",
    "IOP_COPY_SLOW": "显示帧拷贝平均/峰值耗时偏高，可能受内存带宽争用或后台高负载影响。",
    "VBL_JITTER": "VBL 垂直同步中断延迟抖动偏高，关注系统高负载/中断阻塞或显示器刷新稳定性。",
    "WINDOWSERVER_CPU_HIGH": "WindowServer 持续高 CPU 或高内存占用，常见于透明特效、多桌面动效或后台窗口重绘风暴。",
    "CPU_BOTTLENECK": "存在持续满载进程或系统 1 分钟平均负载偏高，建议排查高 CPU 后台任务或切回 balanced/perf CPPC 策略。",
    "MEMORY_PRESSURE": "macOS 内存压力升高，当前无 swap（swap=0）时易触发频繁压缩/回收甚至卡顿。",
    "CPU_THERMAL_HIGH": "CPU 温度持续偏高，建议在 UM790FanBar 切换为平衡或强冷风扇曲线并检查风道散热。",
    "FAN_CPU_HOT": "EC 已将 CPU 风扇切换至高温退避/原厂接管状态，注意降低持续满载并核查散热。",
    "FAN_EC_ANOMALY": "EC 读取失败或风扇出现防震荡退避，建议用 um790fan status --json 检查硬件遥测。",
}


def deep_merge(base, override):
    out = copy.deepcopy(base)
    if not isinstance(override, dict):
        return out
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def ensure_dirs():
    os.makedirs(SUPPORT_DIR, exist_ok=True)
    os.makedirs(LOG_DIR, exist_ok=True)


def write_json_atomic(path, obj):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp = "%s.tmp.%d" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def load_config(path=CONFIG_PATH):
    if not os.path.exists(path):
        return copy.deepcopy(DEFAULT_CONFIG)
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
        return deep_merge(DEFAULT_CONFIG, raw)
    except Exception:
        return copy.deepcopy(DEFAULT_CONFIG)


def save_config(cfg, path=CONFIG_PATH):
    write_json_atomic(path, cfg)


def rotate_file_if_needed(path, max_bytes):
    try:
        if max_bytes > 0 and os.path.exists(path) and os.path.getsize(path) >= max_bytes:
            backup = "%s.1" % path
            if os.path.exists(backup):
                os.remove(backup)
            os.replace(path, backup)
    except OSError:
        pass


def append_event_files(events, max_bytes=5 * 1024 * 1024, jsonl_path=EVENTS_JSONL, log_path=EVENTS_LOG):
    if not events:
        return
    parent = os.path.dirname(jsonl_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    rotate_file_if_needed(jsonl_path, max_bytes)
    rotate_file_if_needed(log_path, max_bytes)
    with open(jsonl_path, "a", encoding="utf-8") as jf:
        for ev in events:
            jf.write(json.dumps(ev, ensure_ascii=False) + "\n")
    with open(log_path, "a", encoding="utf-8") as tf:
        for ev in events:
            tf.write("[%s] [%s/%s] %s | %s — %s\n" % (
                ev.get("ts_iso", ""),
                ev.get("severity", "info").upper(),
                ev.get("category", "sys"),
                ev.get("code", ""),
                ev.get("title", ""),
                ev.get("detail", ""),
            ))


def parse_load1(raw):
    m = LOADAVG_RE.search(raw or "")
    if not m:
        return 0.0
    try:
        return float(m.group(1))
    except ValueError:
        return 0.0


def read_sysctls():
    out = {}
    try:
        p = subprocess.run(
            ["sysctl"] + SYSCTL_KEYS,
            text=True,
            capture_output=True,
            timeout=5,
        )
    except Exception:
        return out
    for line in p.stdout.splitlines():
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        k = k.strip()
        v = v.strip()
        if k == "vm.loadavg":
            out[k] = parse_load1(v)
        else:
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def read_processes():
    res = {
        "pid_map": {},
        "ws_cpu_pct": 0.0,
        "ws_rss_mb": 0.0,
        "chrome_gpu_cpu_pct": 0.0,
        "chrome_gpu_rss_mb": 0.0,
        "top_proc_name": "",
        "top_proc_pid": 0,
        "top_proc_cpu_pct": 0.0,
    }
    try:
        p = subprocess.run(
            ["ps", "-axo", "pid=,pcpu=,rss=,comm="],
            text=True,
            capture_output=True,
            timeout=5,
        )
    except Exception:
        return res
    if p.returncode != 0:
        return res
    for raw in p.stdout.splitlines():
        parts = raw.strip().split(None, 3)
        if len(parts) < 4:
            continue
        try:
            pid = int(parts[0])
            cpu = float(parts[1])
            rss_kb = int(parts[2])
        except ValueError:
            continue
        comm = parts[3].strip()
        name = os.path.basename(comm)
        res["pid_map"][pid] = name
        rss_mb = rss_kb / 1024.0
        if name == "WindowServer":
            res["ws_cpu_pct"] += cpu
            res["ws_rss_mb"] += rss_mb
        else:
            if "Chrome Helper" in comm and ("GPU" in comm or "Aperitif" in comm):
                res["chrome_gpu_cpu_pct"] += cpu
                res["chrome_gpu_rss_mb"] += rss_mb
            if cpu > res["top_proc_cpu_pct"] and name not in ("kernel_task", "idle"):
                res["top_proc_cpu_pct"] = cpu
                res["top_proc_name"] = name
                res["top_proc_pid"] = pid
    res["ws_cpu_pct"] = round(res["ws_cpu_pct"], 1)
    res["ws_rss_mb"] = round(res["ws_rss_mb"], 1)
    res["chrome_gpu_cpu_pct"] = round(res["chrome_gpu_cpu_pct"], 1)
    res["chrome_gpu_rss_mb"] = round(res["chrome_gpu_rss_mb"], 1)
    res["top_proc_cpu_pct"] = round(res["top_proc_cpu_pct"], 1)
    return res


def read_fan_status():
    if not os.path.exists(UM790FAN):
        return None
    try:
        p = subprocess.run(
            [UM790FAN, "status", "--json"],
            text=True,
            capture_output=True,
            timeout=5,
        )
        if p.returncode != 0:
            return None
        return json.loads(p.stdout)
    except Exception:
        return None


def discover_nvmtl_logs():
    candidates = ["/tmp/nvmtl.log"]
    try:
        p = subprocess.run(["getconf", "DARWIN_USER_TEMP_DIR"], text=True, capture_output=True, timeout=3)
        if p.returncode == 0 and p.stdout.strip():
            candidates.append(os.path.join(p.stdout.strip(), "nvmtl.log"))
    except Exception:
        pass
    tmp = os.environ.get("TMPDIR", "")
    if tmp:
        candidates.append(os.path.join(tmp, "nvmtl.log"))
    seen = set()
    out = []
    for c in candidates:
        real = os.path.realpath(c)
        if real not in seen:
            seen.add(real)
            out.append(real)
    return out


class LogTailer:
    """Incrementally tails log files with inode/truncation awareness and UTF-8 safe buffering."""

    def __init__(self, paths=None, start_at_end=True, max_chunk=512 * 1024):
        self.paths = list(paths or [])
        self.max_chunk = max_chunk
        self.state = {}
        for p in self.paths:
            self._init_path(p, start_at_end=start_at_end)

    def _init_path(self, path, start_at_end):
        real = os.path.realpath(path)
        if real in self.state:
            return
        st_ino = None
        offset = 0
        if os.path.exists(real):
            try:
                st = os.stat(real)
                st_ino = st.st_ino
                offset = st.st_size if start_at_end else 0
            except OSError:
                pass
        self.state[real] = {"inode": st_ino, "offset": offset, "buf": b""}

    def add_paths(self, paths, start_at_end=False):
        for p in paths:
            self._init_path(p, start_at_end=start_at_end)

    def poll(self):
        lines = []
        for path, st in list(self.state.items()):
            if not os.path.exists(path):
                continue
            try:
                stat_res = os.stat(path)
            except OSError:
                continue
            if st["inode"] is not None and (stat_res.st_ino != st["inode"] or stat_res.st_size < st["offset"]):
                st["offset"] = 0
                st["buf"] = b""
            st["inode"] = stat_res.st_ino
            if stat_res.st_size <= st["offset"]:
                continue
            to_read = min(stat_res.st_size - st["offset"], self.max_chunk)
            if to_read <= 0:
                continue
            try:
                with open(path, "rb") as f:
                    f.seek(st["offset"])
                    chunk = f.read(to_read)
                st["offset"] += len(chunk)
            except OSError:
                continue
            data = st["buf"] + chunk
            nl = data.rfind(b"\n")
            if nl < 0:
                st["buf"] = data
                continue
            complete = data[:nl]
            st["buf"] = data[nl + 1:]
            for ln in complete.decode("utf-8", "replace").splitlines():
                lines.append((path, ln))
        return lines


def parse_ips_file(path):
    """Parse macOS two-JSON-document .ips crash report safely."""
    fn = os.path.basename(path)
    info = {
        "file": fn,
        "path": path,
        "app_name": fn.split("-")[0],
        "exception": "",
        "termination": "",
        "frames": [],
        "is_gpu": False,
    }
    if not path.endswith(".ips"):
        info["is_gpu"] = any(m in fn for m in GPU_PROCS)
        return info
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            first = f.readline()
            rest = f.read()
        hdr = json.loads(first) if first.strip() else {}
        body = json.loads(rest) if rest.strip() else {}
    except Exception:
        info["is_gpu"] = any(m in fn for m in GPU_PROCS)
        return info

    if isinstance(hdr, dict) and hdr.get("app_name"):
        info["app_name"] = str(hdr["app_name"])
    elif isinstance(body, dict) and body.get("procName"):
        info["app_name"] = str(body["procName"])

    exc = body.get("exception") if isinstance(body, dict) else None
    if isinstance(exc, dict):
        sig = exc.get("signal") or exc.get("type") or ""
        sub = exc.get("subtype") or ""
        info["exception"] = ("%s %s" % (sig, sub)).strip()

    term = body.get("termination") if isinstance(body, dict) else None
    if isinstance(term, dict) and term.get("indicator"):
        info["termination"] = str(term["indicator"])

    ft = body.get("faultingThread") if isinstance(body, dict) else None
    threads = body.get("threads") if isinstance(body, dict) else None
    imgs = body.get("usedImages") if isinstance(body, dict) else None
    frames_out = []
    if isinstance(ft, int) and isinstance(threads, list) and 0 <= ft < len(threads):
        fr_list = threads[ft].get("frames", [])[:4]
        for fr in fr_list:
            ii = fr.get("imageIndex", -1)
            iname = "?"
            if isinstance(imgs, list) and 0 <= ii < len(imgs):
                iname = imgs[ii].get("name") or "?"
            sym = fr.get("symbol") or hex(fr.get("imageOffset", 0))
            off = fr.get("symbolLocation")
            frames_out.append("%s:%s%s" % (iname, sym, ("+%d" % off) if off is not None else ""))
    info["frames"] = frames_out

    app = info["app_name"]
    joined_frames = " ".join(frames_out)
    if any(p in app for p in GPU_PROCS) or any(m in joined_frames for m in GPU_FRAME_MARKERS):
        info["is_gpu"] = True
    return info


class CrashWatcher:
    """Watches DiagnosticReports directories for new crash files without replaying old ones."""

    def __init__(self, dirs=None):
        self.dirs = list(dirs or CRASH_DIRS)
        self.seen = set()
        for d in self.dirs:
            if os.path.isdir(d):
                try:
                    for fn in os.listdir(d):
                        if fn.endswith(CRASH_EXTS):
                            self.seen.add(os.path.join(d, fn))
                except OSError:
                    pass

    def poll(self):
        new_crashes = []
        for d in self.dirs:
            if not os.path.isdir(d):
                continue
            try:
                files = sorted(os.listdir(d))
            except OSError:
                continue
            for fn in files:
                if not fn.endswith(CRASH_EXTS):
                    continue
                fp = os.path.join(d, fn)
                if fp in self.seen:
                    continue
                self.seen.add(fp)
                new_crashes.append(parse_ips_file(fp))
        return new_crashes


class WatchEngine:
    """Pure stateful detection engine operating on snapshots, log lines, crashes, and fan info."""

    def __init__(self, config=None):
        self.config = deep_merge(DEFAULT_CONFIG, config or {})
        self.prev_sysctls = None
        self.prev_ts = None
        self.prev_fan = None
        self.cooldowns = {}
        self.drop_streak = 0
        self.ws_cpu_streak = 0
        self.proc_cpu_streak = 0
        self.load1_streak = 0
        self.temp_streak = 0
        self.gate_windows = {}
        self.reclaim_windows = {}
        self.host_ptr_windows = {}
        self.last_metrics = {}
        self.event_history = deque(maxlen=200)
        self.total_counts = Counter()

    def update_config(self, config):
        self.config = deep_merge(DEFAULT_CONFIG, config or {})

    def _cooldown_sec_for(self, default_cd):
        cfg_cd = float(self.config.get("cooldown_sec", 60))
        return min(default_cd, cfg_cd) if cfg_cd > 0 else default_cd

    def _emit(self, ts, events, severity, category, code, dedup_key, cooldown_sec, title, detail, metrics=None):
        last = self.cooldowns.get(dedup_key)
        if last is not None and (ts - last) < cooldown_sec:
            return
        self.cooldowns[dedup_key] = ts
        ts_iso = datetime.datetime.fromtimestamp(ts).astimezone().isoformat(timespec="seconds")
        ev = {
            "ts": round(ts, 3),
            "ts_iso": ts_iso,
            "severity": severity,
            "category": category,
            "code": code,
            "title": title,
            "detail": detail,
            "hint": REMEDIATION_HINTS.get(code, ""),
        }
        if metrics:
            ev["metrics"] = metrics
        events.append(ev)
        self.event_history.append(ev)
        self.total_counts[severity] += 1

    def _delta(self, cur, prev, key):
        if key not in cur or key not in prev:
            return 0
        d = cur[key] - prev[key]
        return d if d >= 0 else 0

    def process_tick(self, ts, sysctls, procs, log_lines=None, crashes=None, fan=None):
        events = []
        th = self.config.get("thresholds", {})
        pid_map = (procs or {}).get("pid_map", {})
        log_lines = log_lines or []
        crashes = crashes or []

        ws_cpu = float((procs or {}).get("ws_cpu_pct", 0.0))
        ws_rss = float((procs or {}).get("ws_rss_mb", 0.0))
        top_name = (procs or {}).get("top_proc_name", "")
        top_cpu = float((procs or {}).get("top_proc_cpu_pct", 0.0))
        load1 = float((sysctls or {}).get("vm.loadavg", 0.0))
        mem_lvl = int((sysctls or {}).get("kern.memorystatus_vm_pressure_level", 1) or 1)

        active_fps = self.last_metrics.get("active_fps")
        drop_pct = self.last_metrics.get("drop_pct", 0.0)
        iop_copy_avg_us = self.last_metrics.get("iop_copy_avg_us", 0.0)
        vbl_late_avg_us = self.last_metrics.get("vbl_late_avg_us", 0.0)

        # 1. Sysctl delta & spike rules
        if self.prev_sysctls is not None and sysctls:
            prev = self.prev_sysctls
            dt = max(0.1, ts - self.prev_ts) if self.prev_ts is not None else float(self.config.get("interval_sec", 5))

            dhk = self._delta(sysctls, prev, "debug.phx_hang_kills")
            if dhk > 0:
                self._emit(ts, events, "critical", "driver", "GPU_HANG_KILL", "GPU_HANG_KILL", 10,
                           "检测到 GPU Hang 强制重置 (+%d)" % dhk,
                           "debug.phx_hang_kills=%d (+%d)" % (sysctls["debug.phx_hang_kills"], dhk))

            dgto = self._delta(sysctls, prev, "debug.nvaccel_surface_gpu_timeouts")
            if dgto > 0:
                wmax = sysctls.get("debug.nvaccel_surface_gpu_wait_max_us", 0)
                self._emit(ts, events, "critical", "driver", "SURFACE_GPU_TIMEOUT", "SURFACE_GPU_TIMEOUT", 30,
                           "Surface GPU 等待超时 (+%d)" % dgto,
                           "timeouts=%d (+%d), max_wait=%d us" % (sysctls["debug.nvaccel_surface_gpu_timeouts"], dgto, wmax))
            else:
                cur_w = sysctls.get("debug.nvaccel_surface_gpu_wait_max_us", 0)
                prev_w = prev.get("debug.nvaccel_surface_gpu_wait_max_us", cur_w)
                if cur_w > prev_w and cur_w >= int(th.get("gpu_wait_max_us_warn", 25000)):
                    self._emit(ts, events, "warn", "driver", "SURFACE_GPU_WAIT_SPIKE", "SURFACE_GPU_WAIT_SPIKE", 120,
                               "Surface GPU 峰值等待飙升 (%.1f ms)" % (cur_w / 1000.0),
                               "nvaccel_surface_gpu_wait_max_us: %d -> %d us" % (prev_w, cur_w))

            diop_f = self._delta(sysctls, prev, "debug.nvaccel_iop_fail")
            dhdmi = self._delta(sysctls, prev, "debug.phxfb_hdmi_reset")
            dvm_r = self._delta(sysctls, prev, "debug.nvaccel_vm_refused")
            if diop_f > 0 or dhdmi > 0 or dvm_r > 0:
                self._emit(ts, events, "critical", "driver", "IOP_DISPLAY_FAIL", "IOP_DISPLAY_FAIL", 30,
                           "显示/显存驱动异常",
                           "iop_fail+%d, hdmi_reset+%d, vm_refused+%d" % (diop_f, dhdmi, dvm_r))

            dad = self._delta(sysctls, prev, "debug.nvaccel_iop_async_drop")
            dil = self._delta(sysctls, prev, "debug.nvaccel_iop_late")
            dar = self._delta(sysctls, prev, "debug.nvaccel_iop_async_refused")
            if dad > 0 or dil > 0 or dar > 0:
                self._emit(ts, events, "warn", "driver", "IOP_ASYNC_DROP", "IOP_ASYNC_DROP", 60,
                           "异步显示拷贝异常",
                           "async_drop+%d, iop_late+%d, async_refused+%d" % (dad, dil, dar))

            dg1 = self._delta(sysctls, prev, "debug.nvaccel_iop_gap1")
            dg2 = self._delta(sysctls, prev, "debug.nvaccel_iop_gap2")
            dg3 = self._delta(sysctls, prev, "debug.nvaccel_iop_gap3")
            dg4 = self._delta(sysctls, prev, "debug.nvaccel_iop_gap4")
            dtot = dg1 + dg2 + dg3 + dg4
            if dtot > 0:
                active_fps = round(dtot / dt, 1)
                drop_pct = round((dg2 + dg3 + dg4) * 100.0 / dtot, 1)
            else:
                active_fps = 0.0
                drop_pct = 0.0

            min_frames = int(th.get("frame_drop_min_frames", 40))
            drop_thresh = float(th.get("frame_drop_pct_warn", 18.0))
            gap4_thresh = int(th.get("gap4_burst_warn", 6))
            if dtot >= min_frames and (drop_pct >= drop_thresh or dg4 >= gap4_thresh):
                self.drop_streak += 1
                if self.drop_streak >= 2:
                    self._emit(
                        ts, events, "warn", "perf", "FRAME_DROP_BURST", "FRAME_DROP_BURST", 60,
                        "显示掉帧波动 (掉帧 %.1f%% · %.1f FPS)" % (drop_pct, active_fps),
                        "gaps 1x=%d 2x=%d 3x=%d 4x+=%d | WS=%.1f%% | top=%s(%.1f%%)" % (
                            dg1, dg2, dg3, dg4, ws_cpu, top_name or "-", top_cpu),
                        metrics={"drop_pct": drop_pct, "active_fps": active_fps, "ws_cpu_pct": ws_cpu},
                    )
            else:
                self.drop_streak = 0

            diop_ok = self._delta(sysctls, prev, "debug.nvaccel_iop_ok")
            diop_sum = self._delta(sysctls, prev, "debug.nvaccel_iop_copy_us_sum")
            cur_cmax = sysctls.get("debug.nvaccel_iop_copy_us_max", 0)
            prev_cmax = prev.get("debug.nvaccel_iop_copy_us_max", cur_cmax)
            if diop_ok > 0:
                iop_copy_avg_us = round(diop_sum / float(diop_ok), 1)
            if (diop_ok >= 30 and iop_copy_avg_us >= int(th.get("iop_copy_avg_us_warn", 2000))) or (
                cur_cmax > prev_cmax and cur_cmax >= int(th.get("iop_copy_max_us_warn", 4500))
            ):
                self._emit(ts, events, "warn", "perf", "IOP_COPY_SLOW", "IOP_COPY_SLOW", 90,
                           "显示帧拷贝耗时偏高 (avg %.0f us)" % iop_copy_avg_us,
                           "iop_ok+%d, avg=%.0f us, max=%d us" % (diop_ok, iop_copy_avg_us, cur_cmax))

            dvbl_n = self._delta(sysctls, prev, "debug.phxfb_vbl_n")
            dvbl_sum = self._delta(sysctls, prev, "debug.phxfb_vbl_late_us_sum")
            cur_vmax = sysctls.get("debug.phxfb_vbl_late_us_max", 0)
            prev_vmax = prev.get("debug.phxfb_vbl_late_us_max", cur_vmax)
            if dvbl_n > 0:
                vbl_late_avg_us = round(dvbl_sum / float(dvbl_n), 1)
            if (dvbl_n >= 100 and vbl_late_avg_us >= int(th.get("vbl_late_avg_us_warn", 1000))) or (
                cur_vmax > prev_vmax and cur_vmax >= int(th.get("vbl_late_max_us_warn", 10000))
            ):
                self._emit(ts, events, "warn", "perf", "VBL_JITTER", "VBL_JITTER", 90,
                           "VBL 垂直同步延迟抖动 (avg %.0f us)" % vbl_late_avg_us,
                           "vbl+%d, avg=%.0f us, max=%d us" % (dvbl_n, vbl_late_avg_us, cur_vmax))

        if sysctls:
            self.prev_sysctls = dict(sysctls)
            self.prev_ts = ts

        # 2. Crash reports
        for cr in crashes:
            sev = "critical" if cr.get("is_gpu") else "info"
            code = "PROCESS_CRASH_GPU" if cr.get("is_gpu") else "PROCESS_CRASH_OTHER"
            frames_txt = " <- ".join(cr.get("frames", [])[:3])
            detail = "%s | %s | %s" % (
                cr.get("file", ""),
                cr.get("exception") or cr.get("termination") or "unknown",
                frames_txt or "no frames",
            )
            title = "图形相关进程崩溃: %s" % cr.get("app_name", "?") if cr.get("is_gpu") else "进程崩溃: %s" % cr.get("app_name", "?")
            self._emit(ts, events, sev, "driver", code, "crash:%s" % cr.get("file", ""), 5, title, detail)

        # 3. NVMTL log lines
        for _, line in log_lines:
            m = NVMTL_LINE_RE.match(line)
            if m:
                pid = int(m.group(1))
                body = m.group(2)
            else:
                pid = 0
                body = line
            pname = pid_map.get(pid, "pid:%d" % pid) if pid else "unknown"

            if "VK_ERROR_OUT_OF_DEVICE_MEMORY" in body or "VK_ERROR_DEVICE_LOST" in body:
                self._emit(ts, events, "critical", "nvmtl", "RADV_OOM", "RADV_OOM:%d" % pid, 45,
                           "RADV 显存溢出/设备丢失 (%s)" % pname,
                           "pid=%d (%s) | %s" % (pid, pname, body[:200]))
                continue

            err_kind = None
            if "-1000158000" in body:
                err_kind = "vk_create_image_err"
            elif "pass REFUSED" in body:
                err_kind = "pass_refused"
            elif "draw with no pipeline set" in body:
                err_kind = "no_pipeline"
            elif "event-seg" in body:
                err_kind = "event_seg"
            elif "FAILED" in body and "0 prologues failed" not in body:
                err_kind = "failed"
            if err_kind is not None:
                self._emit(ts, events, "warn", "nvmtl", "NVMTL_DRIVER_ERROR", "NVMTL:%d:%s" % (pid, err_kind), 60,
                           "NVMTLDriver 错误 [%s] (%s)" % (err_kind, pname),
                           "pid=%d (%s) | %s" % (pid, pname, body[:200]))

            if any(k in body for k in ("soft VRAM gate", "amd soft-gate", "VRAM wall refused", "spilled")):
                dq = self.gate_windows.setdefault(pid, deque())
                dq.append(ts)
                while dq and dq[0] < ts - 30.0:
                    dq.popleft()
                if len(dq) >= 2:
                    self._emit(ts, events, "warn", "nvmtl", "NVMTL_VRAM_PRESSURE", "VRAM:%d" % pid, 90,
                               "显存软墙/溢出高压 (%s)" % pname,
                               "pid=%d (%s) 30s 内触发 %d 次 soft-gate/wall: %s" % (pid, pname, len(dq), body[:160]))

            if any(k in body for k in ("surface-keep reclaim:", "image park: drained", "buffer park: drained")):
                dq = self.reclaim_windows.setdefault(pid, deque())
                dq.append(ts)
                while dq and dq[0] < ts - 30.0:
                    dq.popleft()
                if len(dq) >= int(th.get("reclaim_burst_warn", 8)):
                    self._emit(ts, events, "warn", "nvmtl", "NVMTL_VRAM_PRESSURE", "VRAM:%d" % pid, 90,
                               "显存池频繁紧急回收 (%s)" % pname,
                               "pid=%d (%s) 30s 内触发 %d 次 reclaim/drain" % (pid, pname, len(dq)))

            if "vkAllocateMemory refused the imported host pointer" in body:
                dq = self.host_ptr_windows.setdefault(pid, deque())
                dq.append(ts)
                while dq and dq[0] < ts - 30.0:
                    dq.popleft()
                if len(dq) >= int(th.get("host_ptr_refused_burst_warn", 20)):
                    self._emit(ts, events, "warn", "nvmtl", "NVMTL_HOST_PTR_BURST", "HOSTPTR:%d" % pid, 120,
                               "宿主机指针导入高频被拒 (%s)" % pname,
                               "pid=%d (%s) 30s 内被拒 %d 次" % (pid, pname, len(dq)))

        # 4. Process / CPU / Memory bottlenecks
        ws_cpu_thresh = float(th.get("ws_cpu_pct_warn", 45.0))
        if ws_cpu >= ws_cpu_thresh:
            self.ws_cpu_streak += 1
        else:
            self.ws_cpu_streak = 0
        ws_rss_thresh = int(th.get("ws_rss_mb_warn", 1536))
        if self.ws_cpu_streak >= 3 or ws_rss >= ws_rss_thresh:
            self._emit(ts, events, "warn", "perf", "WINDOWSERVER_CPU_HIGH", "WINDOWSERVER_CPU_HIGH", 120,
                       "WindowServer 资源占用偏高 (CPU %.1f%% · %.0f MiB)" % (ws_cpu, ws_rss),
                       "ws_cpu=%.1f%% (streak=%d), ws_rss=%.0f MiB" % (ws_cpu, self.ws_cpu_streak, ws_rss))

        if top_cpu >= float(th.get("proc_cpu_pct_warn", 300.0)):
            self.proc_cpu_streak += 1
        else:
            self.proc_cpu_streak = 0
        if load1 >= float(th.get("load1_warn", 12.0)):
            self.load1_streak += 1
        else:
            self.load1_streak = 0
        if self.proc_cpu_streak >= 3 or self.load1_streak >= 3:
            self._emit(ts, events, "warn", "perf", "CPU_BOTTLENECK", "CPU_BOTTLENECK", 120,
                       "CPU 负载瓶颈 (%s %.1f%% · load1 %.2f)" % (top_name or "-", top_cpu, load1),
                       "top=%s(%.1f%%, streak=%d), load1=%.2f (streak=%d)" % (
                           top_name or "-", top_cpu, self.proc_cpu_streak, load1, self.load1_streak))

        if mem_lvl >= 2:
            sev = "critical" if mem_lvl >= 4 else "warn"
            self._emit(ts, events, sev, "perf", "MEMORY_PRESSURE", "MEMORY_PRESSURE:%d" % mem_lvl, 120,
                       "系统内存压力升高 (level=%d)" % mem_lvl,
                       "kern.memorystatus_vm_pressure_level=%d (load1=%.2f)" % (mem_lvl, load1))

        # 5. Thermal & EC fan health (ignore system fan 'hot' flag which is true at 55C)
        cpu_temp = self.last_metrics.get("cpu_temp_c")
        cpu_rpm = self.last_metrics.get("cpu_rpm")
        sys_temp = self.last_metrics.get("sys_temp_c")
        sys_rpm = self.last_metrics.get("sys_rpm")
        if isinstance(fan, dict):
            fans = {f.get("name"): f for f in fan.get("fans", []) if isinstance(f, dict)}
            cfan = fans.get("cpu", {})
            sfan = fans.get("system", {})
            cpu_temp = cfan.get("temp_c", cpu_temp)
            cpu_rpm = cfan.get("rpm", cpu_rpm)
            sys_temp = sfan.get("temp_c", sys_temp)
            sys_rpm = sfan.get("rpm", sys_rpm)

            if cpu_temp is not None and cpu_temp >= int(th.get("cpu_temp_c_warn", 88)):
                self.temp_streak += 1
                if self.temp_streak >= 2:
                    self._emit(ts, events, "warn", "thermal", "CPU_THERMAL_HIGH", "CPU_THERMAL_HIGH", 120,
                               "CPU 温度持续偏高 (%d°C · %s RPM)" % (cpu_temp, cpu_rpm),
                               "cpu_temp=%d°C (streak=%d), cpu_rpm=%s" % (cpu_temp, self.temp_streak, cpu_rpm))
            else:
                self.temp_streak = 0

            if cfan.get("hot") and cpu_temp is not None and cpu_temp >= 80:
                self._emit(ts, events, "warn", "thermal", "FAN_CPU_HOT", "FAN_CPU_HOT", 120,
                           "CPU 风扇进入高温交还状态 (%d°C)" % cpu_temp,
                           "cpu fan hot=True at %d°C, rpm=%s" % (cpu_temp, cpu_rpm))

            if self.prev_fan is not None:
                prev_rf = int(self.prev_fan.get("read_failures", 0) or 0)
                cur_rf = int(fan.get("read_failures", 0) or 0)
                prev_cfan = {f.get("name"): f for f in self.prev_fan.get("fans", []) if isinstance(f, dict)}.get("cpu", {})
                cur_hunts = int(cfan.get("hunts", 0) or 0)
                prev_hunts = int(prev_cfan.get("hunts", 0) or 0)
                if cur_rf > prev_rf or (cur_hunts - prev_hunts) >= 3:
                    self._emit(ts, events, "warn", "thermal", "FAN_EC_ANOMALY", "FAN_EC_ANOMALY", 120,
                               "EC 风扇遥测/震荡异常",
                               "read_failures: %d->%d, cpu_hunts: %d->%d" % (prev_rf, cur_rf, prev_hunts, cur_hunts))
            self.prev_fan = fan

        self.last_metrics = {
            "ws_cpu_pct": ws_cpu,
            "ws_rss_mb": ws_rss,
            "chrome_gpu_cpu_pct": (procs or {}).get("chrome_gpu_cpu_pct", 0.0),
            "chrome_gpu_rss_mb": (procs or {}).get("chrome_gpu_rss_mb", 0.0),
            "top_proc_name": top_name,
            "top_proc_cpu_pct": top_cpu,
            "load1": round(load1, 2),
            "mem_pressure": mem_lvl,
            "active_fps": active_fps,
            "drop_pct": drop_pct,
            "iop_copy_avg_us": iop_copy_avg_us,
            "vbl_late_avg_us": vbl_late_avg_us,
            "cpu_temp_c": cpu_temp,
            "cpu_rpm": cpu_rpm,
            "sys_temp_c": sys_temp,
            "sys_rpm": sys_rpm,
            "hang_kills_total": (sysctls or {}).get("debug.phx_hang_kills", 0),
            "gpu_timeouts_total": (sysctls or {}).get("debug.nvaccel_surface_gpu_timeouts", 0),
            "iop_fail_total": (sysctls or {}).get("debug.nvaccel_iop_fail", 0),
        }
        return events


def load_events_from_jsonl(hours=24.0, jsonl_path=EVENTS_JSONL):
    cutoff = (time.time() - hours * 3600.0) if hours and hours > 0 else 0.0
    events = []
    for p in (jsonl_path + ".1", jsonl_path):
        if not os.path.exists(p):
            continue
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    s = line.strip()
                    if not s:
                        continue
                    try:
                        ev = json.loads(s)
                        if ev.get("ts", 0.0) >= cutoff:
                            events.append(ev)
                    except json.JSONDecodeError:
                        continue
        except OSError:
            continue
    return events


def count_unread_events(ack_ts, jsonl_path=EVENTS_JSONL):
    crit = 0
    warn = 0
    for ev in load_events_from_jsonl(hours=0, jsonl_path=jsonl_path):
        if ev.get("ts", 0.0) <= ack_ts:
            continue
        sev = ev.get("severity")
        if sev == "critical":
            crit += 1
        elif sev == "warn":
            warn += 1
    return crit, warn


def send_notification(title, message):
    script = "on run argv\ndisplay notification (item 2 of argv) with title (item 1 of argv)\nend run"
    try:
        subprocess.run(
            ["osascript", "-e", script, str(title), str(message)[:180]],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except Exception:
        pass


def build_status_snapshot(cfg, engine, running, started_at, pid=None):
    ack_ts = float(cfg.get("ack_ts", 0.0))
    all_recent = load_events_from_jsonl(hours=24.0)
    unread_crit = sum(1 for e in all_recent if e.get("ts", 0.0) > ack_ts and e.get("severity") == "critical")
    unread_warn = sum(1 for e in all_recent if e.get("ts", 0.0) > ack_ts and e.get("severity") == "warn")
    health = "off"
    if running:
        if unread_crit > 0:
            health = "critical"
        elif unread_warn > 0:
            health = "warn"
        else:
            health = "ok"
    return {
        "enabled": bool(cfg.get("enabled", True)),
        "notify": bool(cfg.get("notify", True)),
        "running": bool(running),
        "pid": pid if running else 0,
        "started_at": started_at,
        "updated_at": round(time.time(), 3),
        "health": health,
        "unread_critical": unread_crit,
        "unread_warn": unread_warn,
        "recent_events": all_recent[-8:][::-1],
        "metrics": engine.last_metrics if engine else {},
    }


def render_report_markdown(events, status_obj, hours):
    now_str = datetime.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %z")
    lines = [
        "# UM790 Pro 驱动与性能常驻检测报告",
        "",
        "- 生成时间: `%s`" % now_str,
        "- 统计窗口: 最近 `%.1f` 小时" % hours,
        "- 检测状态: `running=%s`, `enabled=%s`, `health=%s`, `unread_critical=%s`, `unread_warn=%s`" % (
            status_obj.get("running"), status_obj.get("enabled"),
            status_obj.get("health"), status_obj.get("unread_critical"), status_obj.get("unread_warn")
        ),
        "",
        "## 当前实时指标",
        "",
    ]
    m = status_obj.get("metrics") or {}
    if m:
        lines.append("- WindowServer: CPU `%.1f%%`, RSS `%.0f MiB`" % (m.get("ws_cpu_pct", 0.0), m.get("ws_rss_mb", 0.0)))
        lines.append("- 显示刷新与拷贝: activeFPS `%s`, 掉帧率 `%.1f%%`, copy_avg `%.0f us`, vbl_avg `%.0f us`" % (
            m.get("active_fps"), m.get("drop_pct", 0.0), m.get("iop_copy_avg_us", 0.0), m.get("vbl_late_avg_us", 0.0)
        ))
        lines.append("- CPU/内存: top `%s (%.1f%%)`, load1 `%.2f`, 内存压力 `%s`" % (
            m.get("top_proc_name") or "-", m.get("top_proc_cpu_pct", 0.0), m.get("load1", 0.0), m.get("mem_pressure", 1)
        ))
        lines.append("- 温度/风扇: CPU `%s°C (%s RPM)`, 系统 `%s°C (%s RPM)`" % (
            m.get("cpu_temp_c"), m.get("cpu_rpm"), m.get("sys_temp_c"), m.get("sys_rpm")
        ))
        lines.append("- 累计内核异常计数: `hang_kills=%s`, `gpu_timeouts=%s`, `iop_fail=%s`" % (
            m.get("hang_kills_total", 0), m.get("gpu_timeouts_total", 0), m.get("iop_fail_total", 0)
        ))
    else:
        lines.append("- 暂无实时指标（请先执行 `um790watch on` 启动检测）。")

    lines.extend(["", "## 异常分布统计（共 %d 条）" % len(events), ""])
    if not events:
        lines.append("- 统计窗口内无异常事件。")
    else:
        counts = Counter((e.get("severity", "info"), e.get("category", "?"), e.get("code", "?")) for e in events)
        lines.append("| 级别 | 类别 | 事件码 | 次数 | 建议 |")
        lines.append("| :--- | :--- | :--- | ---: | :--- |")
        for (sev, cat, code), n in counts.most_common():
            lines.append("| `%s` | `%s` | `%s` | %d | %s |" % (
                sev, cat, code, n, REMEDIATION_HINTS.get(code, "")
            ))
        lines.extend(["", "## 最近事件明细（最多 15 条）", ""])
        for ev in reversed(events[-15:]):
            lines.append("- `[%s]` **`%s/%s`**: %s — `%s`" % (
                ev.get("ts_iso", ""), ev.get("severity", ""), ev.get("code", ""),
                ev.get("title", ""), ev.get("detail", "")
            ))
    lines.append("")
    return "\n".join(lines)


def read_live_status():
    cfg = load_config()
    st = {}
    if os.path.exists(STATUS_PATH):
        try:
            with open(STATUS_PATH, "r", encoding="utf-8") as f:
                st = json.load(f)
        except Exception:
            st = {}
    pid = int(st.get("pid") or 0)
    alive = False
    if pid > 0:
        try:
            os.kill(pid, 0)
            alive = True
        except OSError:
            alive = False
    ack_ts = float(cfg.get("ack_ts", 0.0))
    unread_crit, unread_warn = count_unread_events(ack_ts)
    if not alive:
        st["running"] = False
        st["pid"] = 0
        st["health"] = "off" if not cfg.get("enabled", True) else "stopped"
    else:
        if unread_crit > 0:
            st["health"] = "critical"
        elif unread_warn > 0:
            st["health"] = "warn"
        else:
            st["health"] = "ok"
    st["enabled"] = bool(cfg.get("enabled", True))
    st["notify"] = bool(cfg.get("notify", True))
    st["unread_critical"] = unread_crit
    st["unread_warn"] = unread_warn
    return st


def ensure_launchd_plist():
    if os.path.exists(PLIST_TEMPLATE):
        with open(PLIST_TEMPLATE, "r", encoding="utf-8") as f:
            content = f.read()
    else:
        content = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>Label</key>
\t<string>%s</string>
\t<key>ProgramArguments</key>
\t<array>
\t\t<string>/usr/bin/python3</string>
\t\t<string>%s</string>
\t\t<string>daemon</string>
\t</array>
\t<key>RunAtLoad</key>
\t<true/>
\t<key>KeepAlive</key>
\t<dict>
\t\t<key>SuccessfulExit</key>
\t\t<false/>
\t</dict>
\t<key>ProcessType</key>
\t<string>Background</string>
\t<key>Nice</key>
\t<integer>10</integer>
\t<key>StandardOutPath</key>
\t<string>%s</string>
\t<key>StandardErrorPath</key>
\t<string>%s</string>
</dict>
</plist>
""" % (LAUNCHD_LABEL, SCRIPT_PATH, DAEMON_LOG, DAEMON_LOG)
    content = content.replace("__LOG_DIR__", LOG_DIR).replace("__SCRIPT_PATH__", SCRIPT_PATH)
    parent = os.path.dirname(LAUNCHD_PLIST)
    os.makedirs(parent, exist_ok=True)
    if os.path.exists(LAUNCHD_PLIST):
        try:
            with open(LAUNCHD_PLIST, "r", encoding="utf-8") as f:
                if f.read() == content:
                    return
        except OSError:
            pass
    with open(LAUNCHD_PLIST, "w", encoding="utf-8") as f:
        f.write(content)


def launchd_target():
    return "gui/%d/%s" % (os.getuid(), LAUNCHD_LABEL)


def launchd_domain():
    return "gui/%d" % os.getuid()


def cmd_on(_args):
    ensure_dirs()
    cfg = load_config()
    cfg["enabled"] = True
    save_config(cfg)
    ensure_launchd_plist()
    subprocess.run(["launchctl", "bootout", launchd_target()], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p = subprocess.run(["launchctl", "bootstrap", launchd_domain(), LAUNCHD_PLIST], text=True, capture_output=True)
    if p.returncode != 0:
        print("launchctl bootstrap failed: %s" % p.stderr.strip(), file=sys.stderr)
        return 1
    for _ in range(25):
        st = read_live_status()
        if st.get("running"):
            print("um790watch ON (pid=%s, notify=%s)" % (st.get("pid"), st.get("notify")))
            return 0
        time.sleep(0.1)
    st = read_live_status()
    print("um790watch enabled (running=%s, pid=%s)" % (st.get("running"), st.get("pid")))
    return 0 if st.get("running") else 1


def cmd_off(_args):
    ensure_dirs()
    cfg = load_config()
    cfg["enabled"] = False
    save_config(cfg)
    subprocess.run(["launchctl", "bootout", launchd_target()], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(15):
        st = read_live_status()
        if not st.get("running"):
            break
        time.sleep(0.1)
    st = read_live_status()
    st["running"] = False
    st["enabled"] = False
    st["health"] = "off"
    st["updated_at"] = round(time.time(), 3)
    write_json_atomic(STATUS_PATH, st)
    print("um790watch OFF (running=False)")
    return 0


def cmd_toggle(args):
    st = read_live_status()
    if st.get("enabled") and st.get("running"):
        return cmd_off(args)
    return cmd_on(args)


def signal_daemon_reload(cfg):
    st = read_live_status()
    pid = int(st.get("pid") or 0)
    if st.get("running") and pid > 0:
        try:
            os.kill(pid, signal.SIGUSR1)
            time.sleep(0.15)
            return
        except OSError:
            pass
    st["notify"] = bool(cfg.get("notify", True))
    st["enabled"] = bool(cfg.get("enabled", True))
    st["updated_at"] = round(time.time(), 3)
    write_json_atomic(STATUS_PATH, st)


def cmd_notify(args):
    ensure_dirs()
    cfg = load_config()
    cfg["notify"] = (args.state == "on")
    save_config(cfg)
    signal_daemon_reload(cfg)
    print("um790watch notify=%s" % cfg["notify"])
    return 0


def cmd_ack(_args):
    ensure_dirs()
    cfg = load_config()
    cfg["ack_ts"] = time.time()
    save_config(cfg)
    signal_daemon_reload(cfg)
    print("um790watch unread alerts cleared (ack_ts=%.0f)" % cfg["ack_ts"])
    return 0


def cmd_clear(_args):
    ensure_dirs()
    for p in (EVENTS_JSONL, EVENTS_JSONL + ".1", EVENTS_LOG, EVENTS_LOG + ".1"):
        try:
            if os.path.exists(p):
                with open(p, "w", encoding="utf-8"):
                    pass
        except OSError:
            pass
    cfg = load_config()
    cfg["ack_ts"] = time.time()
    save_config(cfg)
    signal_daemon_reload(cfg)
    print("um790watch logs and alerts cleared")
    return 0


def cmd_status(args):
    st = read_live_status()
    if args.json:
        print(json.dumps(st, indent=2, ensure_ascii=False))
        return 0
    m = st.get("metrics") or {}
    print("=== um790watch Status ===")
    print("  Enabled : %s | Running: %s (pid=%s) | Notify: %s" % (
        st.get("enabled"), st.get("running"), st.get("pid"), st.get("notify")
    ))
    print("  Health  : %s | Unread: %s critical, %s warn" % (
        st.get("health"), st.get("unread_critical", 0), st.get("unread_warn", 0)
    ))
    if m:
        print("  Display : FPS=%s | Drop=%.1f%% | copy_avg=%.0f us | vbl_avg=%.0f us" % (
            m.get("active_fps"), m.get("drop_pct", 0.0), m.get("iop_copy_avg_us", 0.0), m.get("vbl_late_avg_us", 0.0)
        ))
        print("  Sys/GPU : WS=%.1f%% (%.0f MiB) | top=%s (%.1f%%) | load1=%.2f | mem_pressure=%s" % (
            m.get("ws_cpu_pct", 0.0), m.get("ws_rss_mb", 0.0),
            m.get("top_proc_name") or "-", m.get("top_proc_cpu_pct", 0.0),
            m.get("load1", 0.0), m.get("mem_pressure", 1)
        ))
        print("  Thermal : CPU=%sC (%s RPM) | Sys=%sC (%s RPM)" % (
            m.get("cpu_temp_c"), m.get("cpu_rpm"), m.get("sys_temp_c"), m.get("sys_rpm")
        ))
    return 0


def cmd_report(args):
    ensure_dirs()
    events = load_events_from_jsonl(hours=args.hours)
    st = read_live_status()
    md = render_report_markdown(events, st, args.hours)
    if args.save:
        with open(LATEST_REPORT_MD, "w", encoding="utf-8") as f:
            f.write(md)
        print(LATEST_REPORT_MD)
    else:
        print(md)
    return 0


def cmd_tail(args):
    events = load_events_from_jsonl(hours=0)
    if not events:
        print("暂无异常事件记录。")
        return 0
    for ev in events[-args.lines:]:
        print("[%s] [%s/%s] %s | %s — %s" % (
            ev.get("ts_iso", ""), ev.get("severity", "").upper(), ev.get("category", ""),
            ev.get("code", ""), ev.get("title", ""), ev.get("detail", "")
        ))
    return 0


def cmd_once(args):
    cfg = load_config()
    engine = WatchEngine(cfg)
    tailer = LogTailer(discover_nvmtl_logs(), start_at_end=True)
    crashes = CrashWatcher()
    engine.process_tick(time.time(), read_sysctls(), read_processes(), [], [], read_fan_status())
    time.sleep(1.0)
    evs = engine.process_tick(
        time.time(),
        read_sysctls(),
        read_processes(),
        tailer.poll(),
        crashes.poll(),
        read_fan_status(),
    )
    out = {"events": evs, "metrics": engine.last_metrics}
    if args.json:
        print(json.dumps(out, indent=2, ensure_ascii=False))
    else:
        print("Metrics:", json.dumps(engine.last_metrics, ensure_ascii=False))
        print("Events (%d):" % len(evs), evs)
    return 0


def cmd_daemon(_args):
    ensure_dirs()
    cfg = load_config()
    if not cfg.get("enabled", True):
        st = build_status_snapshot(cfg, None, False, 0, pid=0)
        write_json_atomic(STATUS_PATH, st)
        return 0

    engine = WatchEngine(cfg)
    log_paths = discover_nvmtl_logs()
    tailer = LogTailer(log_paths, start_at_end=True)
    crash_watcher = CrashWatcher()
    started_at = round(time.time(), 3)
    stop_flag = {"stop": False}
    reload_flag = {"reload": False}

    def on_term(_sig, _frame):
        stop_flag["stop"] = True

    def on_usr1(_sig, _frame):
        reload_flag["reload"] = True

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    signal.signal(signal.SIGUSR1, on_usr1)

    last_cfg_mtime = os.path.getmtime(CONFIG_PATH) if os.path.exists(CONFIG_PATH) else 0.0
    last_notify_ts = 0.0
    last_status_flush = 0.0
    tick = 0

    try:
        while not stop_flag["stop"]:
            cur_mtime = os.path.getmtime(CONFIG_PATH) if os.path.exists(CONFIG_PATH) else 0.0
            if reload_flag["reload"] or cur_mtime != last_cfg_mtime:
                reload_flag["reload"] = False
                last_cfg_mtime = cur_mtime
                cfg = load_config()
                engine.update_config(cfg)
                if not cfg.get("enabled", True):
                    break
                write_json_atomic(STATUS_PATH, build_status_snapshot(cfg, engine, True, started_at, pid=os.getpid()))
                last_status_flush = time.time()

            now = time.time()
            sysctls = read_sysctls()
            procs = read_processes()
            if tick % 12 == 0:
                tailer.add_paths(discover_nvmtl_logs(), start_at_end=False)
            lines = tailer.poll()
            crashes = crash_watcher.poll()
            fan = read_fan_status() if (tick % 2 == 0) else None

            events = engine.process_tick(now, sysctls, procs, lines, crashes, fan)
            if events:
                append_event_files(events, max_bytes=int(cfg.get("log_max_bytes", 5 * 1024 * 1024)))
                if cfg.get("notify", True) and (now - last_notify_ts) >= 30.0:
                    min_sev = cfg.get("notify_min_severity", "critical")
                    allowed = ("critical",) if min_sev == "critical" else ("critical", "warn")
                    pick = next((e for e in events if e.get("severity") in allowed), None)
                    if pick is not None:
                        last_notify_ts = now
                        send_notification("UM790 Watch: %s" % pick["title"], pick["detail"])

            if events or (now - last_status_flush) >= 10.0:
                write_json_atomic(STATUS_PATH, build_status_snapshot(cfg, engine, True, started_at, pid=os.getpid()))
                last_status_flush = now

            tick += 1
            sleep_sec = max(1, int(cfg.get("interval_sec", 5)))
            for _ in range(sleep_sec * 5):
                if stop_flag["stop"] or reload_flag["reload"]:
                    break
                time.sleep(0.2)
    finally:
        cfg = load_config()
        st = build_status_snapshot(cfg, engine, False, started_at, pid=0)
        if not cfg.get("enabled", True):
            st["health"] = "off"
        write_json_atomic(STATUS_PATH, st)
    return 0


def build_parser():
    p = argparse.ArgumentParser(prog="um790watch", description="UM790 Pro Hackintosh resident driver & performance monitor")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("on", help="Enable and start background watcher").set_defaults(func=cmd_on)
    sub.add_parser("off", help="Disable and stop background watcher").set_defaults(func=cmd_off)
    sub.add_parser("toggle", help="Toggle background watcher on/off").set_defaults(func=cmd_toggle)

    pn = sub.add_parser("notify", help="Enable or disable desktop notifications")
    pn.add_argument("state", choices=["on", "off"])
    pn.set_defaults(func=cmd_notify)

    ps = sub.add_parser("status", help="Show live watcher status and metrics")
    ps.add_argument("--json", action="store_true")
    ps.set_defaults(func=cmd_status)

    pr = sub.add_parser("report", help="Generate diagnostic report from logged events")
    pr.add_argument("--hours", type=float, default=24.0)
    pr.add_argument("--save", action="store_true")
    pr.set_defaults(func=cmd_report)

    pt = sub.add_parser("tail", help="Show recent events")
    pt.add_argument("-n", "--lines", type=int, default=20)
    pt.set_defaults(func=cmd_tail)

    sub.add_parser("ack", help="Acknowledge unread alerts").set_defaults(func=cmd_ack)
    sub.add_parser("clear", help="Clear logged events and unread counters").set_defaults(func=cmd_clear)

    po = sub.add_parser("once", help="Run a single foreground 1s sample without modifying daemon state")
    po.add_argument("--json", action="store_true")
    po.set_defaults(func=cmd_once)

    sub.add_parser("daemon", help="Run resident monitoring loop (managed by launchd)").set_defaults(func=cmd_daemon)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
