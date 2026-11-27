#!/usr/bin/env python3
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import um790watch as w


class TestUM790Watch(unittest.TestCase):
    def _base_sys(self, **overrides):
        d = {
            "debug.phx_hang_kills": 0,
            "debug.nvaccel_surface_gpu_timeouts": 0,
            "debug.nvaccel_surface_gpu_wait_max_us": 1000,
            "debug.nvaccel_iop_fail": 0,
            "debug.nvaccel_iop_ok": 1000,
            "debug.nvaccel_iop_gap1": 1000,
            "debug.nvaccel_iop_gap2": 10,
            "debug.nvaccel_iop_gap3": 0,
            "debug.nvaccel_iop_gap4": 0,
            "debug.nvaccel_iop_copy_us": 300,
            "debug.nvaccel_iop_copy_us_max": 1000,
            "debug.nvaccel_iop_copy_us_sum": 300000,
            "debug.nvaccel_iop_async_refused": 0,
            "debug.nvaccel_iop_async_drop": 0,
            "debug.nvaccel_iop_late": 0,
            "debug.nvaccel_vm_refused": 0,
            "debug.phxfb_vbl_n": 2000,
            "debug.phxfb_vbl_late_us_sum": 200000,
            "debug.phxfb_vbl_late_us_max": 1200,
            "debug.phxfb_hdmi_reset": 0,
            "kern.memorystatus_vm_pressure_level": 1,
            "vm.loadavg": 2.0,
        }
        d.update(overrides)
        return d

    def _base_procs(self, ws_cpu=5.0, ws_rss=400.0, top_name="chrome", top_cpu=25.0):
        return {
            "pid_map": {203: "WindowServer", 841: "Google Chrome Helper (GPU)"},
            "ws_cpu_pct": ws_cpu,
            "ws_rss_mb": ws_rss,
            "chrome_gpu_cpu_pct": 10.0,
            "chrome_gpu_rss_mb": 200.0,
            "top_proc_name": top_name,
            "top_proc_pid": 999,
            "top_proc_cpu_pct": top_cpu,
        }

    def test_ips_crash_parsing(self):
        with tempfile.TemporaryDirectory() as td:
            ws_ips = os.path.join(td, "WindowServer-2026-11-27.ips")
            with open(ws_ips, "w", encoding="utf-8") as f:
                f.write(json.dumps({"app_name": "WindowServer"}) + "\n")
                f.write(json.dumps({
                    "faultingThread": 0,
                    "exception": {"signal": "SIGSEGV", "subtype": "KERN_INVALID_ADDRESS"},
                    "usedImages": [{"name": "libvulkan_radeon.dylib"}, {"name": "NVMTLDriver"}],
                    "threads": [{"frames": [
                        {"imageIndex": 0, "symbol": "radv_destroy_cmd_buffer", "symbolLocation": 879},
                        {"imageIndex": 1, "symbol": "nvmtl_vk_cmd_begin", "symbolLocation": 551},
                    ]}],
                }))
            parsed = w.parse_ips_file(ws_ips)
            self.assertTrue(parsed["is_gpu"])
            self.assertEqual(parsed["app_name"], "WindowServer")
            self.assertIn("libvulkan_radeon.dylib:radv_destroy_cmd_buffer+879", parsed["frames"])

            notes_ips = os.path.join(td, "Notes-2026-11-27.ips")
            with open(notes_ips, "w", encoding="utf-8") as f:
                f.write(json.dumps({"app_name": "Notes"}) + "\n")
                f.write(json.dumps({
                    "faultingThread": 0,
                    "exception": {"signal": "SIGABRT"},
                    "usedImages": [{"name": "AppKit"}],
                    "threads": [{"frames": [{"imageIndex": 0, "symbol": "NSApplicationMain", "symbolLocation": 10}]}],
                }))
            parsed2 = w.parse_ips_file(notes_ips)
            self.assertFalse(parsed2["is_gpu"])

    def test_log_tailer_and_rotation(self):
        with tempfile.TemporaryDirectory() as td:
            lp = os.path.join(td, "nvmtl.log")
            with open(lp, "w", encoding="utf-8") as f:
                f.write("pid 203 NVMTL: old FAILED line before daemon\n")
            tailer = w.LogTailer([lp], start_at_end=True)
            self.assertEqual(tailer.poll(), [])

            with open(lp, "a", encoding="utf-8") as f:
                f.write("pid 841 NVMTL: 0 prologues failed\n")
                f.write("pid 841 NVMTL: nvk-reason: W radv_cmd_buffer.c: VK_ERROR_OUT_OF_DEVICE_MEMORY\n")
            got = tailer.poll()
            self.assertEqual(len(got), 2)

            # Truncation / rotation resets offset
            with open(lp, "w", encoding="utf-8") as f:
                f.write("pid 203 NVMTL: begin_render: pass REFUSED\n")
            got2 = tailer.poll()
            self.assertEqual(len(got2), 1)
            self.assertIn("pass REFUSED", got2[0][1])

    def test_nvmtl_rules_and_false_positive_guards(self):
        eng = w.WatchEngine()
        procs = self._base_procs()
        eng.process_tick(100.0, self._base_sys(), procs)

        # 0 prologues failed + single host ptr refused must NOT trigger
        evs = eng.process_tick(105.0, self._base_sys(), procs, log_lines=[
            ("/tmp/nvmtl.log", "pid 841 NVMTL: 100 submits; 0 prologues failed"),
            ("/tmp/nvmtl.log", "pid 841 NVMTL: vk: vkAllocateMemory refused the imported host pointer 0x1000"),
        ])
        self.assertEqual(evs, [])

        # VK_ERROR_OUT_OF_DEVICE_MEMORY triggers critical RADV_OOM
        evs = eng.process_tick(110.0, self._base_sys(), procs, log_lines=[
            ("/tmp/nvmtl.log", "pid 841 NVMTL: nvk-reason: W radv_cmd_buffer.c: VK_ERROR_OUT_OF_DEVICE_MEMORY"),
        ])
        self.assertEqual([e["code"] for e in evs], ["RADV_OOM"])
        self.assertEqual(evs[0]["severity"], "critical")

        # 8 reclaims in 30s -> NVMTL_VRAM_PRESSURE; 20 host ptr refused -> NVMTL_HOST_PTR_BURST
        burst = [("x", "pid 841 NVMTL: surface-keep reclaim: freed 10 MB")] * 8
        burst += [("x", "pid 841 NVMTL: vk: vkAllocateMemory refused the imported host pointer 0x2000")] * 20
        evs2 = eng.process_tick(115.0, self._base_sys(), procs, log_lines=burst)
        codes = set(e["code"] for e in evs2)
        self.assertIn("NVMTL_VRAM_PRESSURE", codes)
        self.assertIn("NVMTL_HOST_PTR_BURST", codes)

    def test_sysctl_deltas_and_idle_frame_drop_guard(self):
        eng = w.WatchEngine()
        procs = self._base_procs()
        eng.process_tick(100.0, self._base_sys(), procs)

        # Idle desktop with only 10 frames and high drop ratio must NOT alert
        evs_idle = eng.process_tick(105.0, self._base_sys(
            **{"debug.nvaccel_iop_gap1": 1005, "debug.nvaccel_iop_gap2": 15}
        ), procs)
        self.assertEqual(evs_idle, [])

        # Two consecutive ticks of >=40 frames and >=18% drop triggers FRAME_DROP_BURST
        evs1 = eng.process_tick(110.0, self._base_sys(
            **{"debug.nvaccel_iop_gap1": 1070, "debug.nvaccel_iop_gap2": 40}
        ), procs)
        self.assertEqual(evs1, [])
        evs2 = eng.process_tick(115.0, self._base_sys(
            **{"debug.nvaccel_iop_gap1": 1135, "debug.nvaccel_iop_gap2": 70}
        ), procs)
        self.assertEqual([e["code"] for e in evs2], ["FRAME_DROP_BURST"])

        # Hang kill, timeout, iop_fail, wait spike
        evs_drv = eng.process_tick(120.0, self._base_sys(
            **{
                "debug.nvaccel_iop_gap1": 1200,
                "debug.nvaccel_iop_gap2": 70,
                "debug.phx_hang_kills": 1,
                "debug.nvaccel_surface_gpu_timeouts": 1,
                "debug.nvaccel_iop_fail": 1,
                "kern.memorystatus_vm_pressure_level": 4,
            }
        ), procs)
        codes = set(e["code"] for e in evs_drv)
        self.assertEqual(codes, {"GPU_HANG_KILL", "SURFACE_GPU_TIMEOUT", "IOP_DISPLAY_FAIL", "MEMORY_PRESSURE"})

    def test_ws_cpu_and_thermal_rules(self):
        eng = w.WatchEngine()
        # System fan hot=True at 55C must NOT alert
        fan_normal = {
            "read_failures": 0,
            "fans": [
                {"name": "cpu", "temp_c": 65, "rpm": 2600, "hot": False, "hunts": 0},
                {"name": "system", "temp_c": 55, "rpm": 2050, "hot": True, "hunts": 0},
            ],
        }
        # Normal 120Hz UI compositing (ws_cpu=55%) must NOT trigger warning
        for i in range(3):
            evs_ok = eng.process_tick(50.0 + i * 5, self._base_sys(), self._base_procs(ws_cpu=55.0), fan=fan_normal if i == 0 else None)
            self.assertEqual(evs_ok, [])

        for i in range(3):
            evs = eng.process_tick(100.0 + i * 5, self._base_sys(), self._base_procs(ws_cpu=110.0), fan=None)
            if i < 2:
                self.assertEqual(evs, [])
            else:
                self.assertEqual([e["code"] for e in evs], ["WINDOWSERVER_CPU_HIGH"])

        # CPU thermal 89C for 2 consecutive fan readings -> CPU_THERMAL_HIGH
        fan_hot = {
            "read_failures": 0,
            "fans": [
                {"name": "cpu", "temp_c": 89, "rpm": 4500, "hot": True, "hunts": 0},
                {"name": "system", "temp_c": 55, "rpm": 2050, "hot": True, "hunts": 0},
            ],
        }
        evs_t1 = eng.process_tick(200.0, self._base_sys(), self._base_procs(), fan=fan_hot)
        self.assertEqual([e["code"] for e in evs_t1], ["FAN_CPU_HOT"])
        evs_t2 = eng.process_tick(210.0, self._base_sys(), self._base_procs(), fan=fan_hot)
        self.assertIn("CPU_THERMAL_HIGH", [e["code"] for e in evs_t2])

    def test_rotation_unread_and_report(self):
        with tempfile.TemporaryDirectory() as td:
            jl = os.path.join(td, "events.jsonl")
            tl = os.path.join(td, "events.log")
            evs = [
                {"ts": 100.0, "ts_iso": "t1", "severity": "critical", "category": "driver", "code": "GPU_HANG_KILL", "title": "T1", "detail": "D1"},
                {"ts": 200.0, "ts_iso": "t2", "severity": "warn", "category": "perf", "code": "FRAME_DROP_BURST", "title": "T2", "detail": "D2"},
            ]
            w.append_event_files(evs, max_bytes=1024 * 1024, jsonl_path=jl, log_path=tl)
            self.assertEqual(w.count_unread_events(0.0, jsonl_path=jl), (1, 1))
            self.assertEqual(w.count_unread_events(150.0, jsonl_path=jl), (0, 1))

            md = w.render_report_markdown(evs, {"running": True, "enabled": True, "health": "critical", "unread_critical": 1, "unread_warn": 1, "metrics": {"ws_cpu_pct": 12.0}}, 24.0)
            self.assertIn("GPU_HANG_KILL", md)
            self.assertIn("FRAME_DROP_BURST", md)


if __name__ == "__main__":
    unittest.main()
