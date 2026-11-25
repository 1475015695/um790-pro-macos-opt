# um790-pro-macos-opt

Minisforum UM790 Pro Hackintosh 专项性能优化合集，聚焦 **AMD Zen 4 单核睿频解锁、IT5571 EC 双风扇控制、`NVMTLDriver` Metal→Vulkan 转译与显存调度优化**，以及**零重启基准压测审计套件**。

---

## 硬件与系统环境

| 项目 | 规格 |
| :--- | :--- |
| **主机 / 主板** | Minisforum UM790 Pro (`F7BSC`) |
| **处理器** | AMD Ryzen 9 7940HS (Zen 4 Phoenix, 8C / 16T) |
| **核显** | AMD Radeon 780M (`1002:15bf`, GFX11.0.3 / DCN 3.1.4, BIOS 2 GB UMA) |
| **内存** | 32 GB DDR5 |
| **系统 / SMBIOS** | macOS 26.7.2 (`25G332`), `MacBookPro16,1` |

---

## 核心优化一览（三大模块）

### 1. `um790-fan/`：IT5571 EC 原生双风扇控制 + AMD Zen 4 CPPC 自主睿频驱动
- **`UM790Fan.kext` + `um790fan` CLI + `UM790FanBar.app` 菜单栏控制端**
  - 通过 ITE IT5571 EC 直接监控温度、双风扇转速并支持自动/平衡/静音曲线与手动 RPM 档位调节。
  - **Zen 4 CPPC 自主模式（Autonomous Active Mode）**：通过写入 `MSR_AMD_CPPC_ENABLE (0xC00102B1)` 与 `MSR_AMD_CPPC_REQ (0xC00102B3)`（`CAP1 15/41/149/214`）打通 macOS 下缺失的 CPPC 调度提示，解决轻载/单核被 SMU 误锁在 ~1.65 GHz 的问题，并解锁 `4.8–5.1 GHz` 睿频。
  - 支持免重启实时切换策略：`perf`（`EPP 0x00`）、`balanced`（`EPP 0x20`）、`powersave`（`EPP 0xA0`）或自定义 `epp <0..255>`。
  - **安全启动逃生参数（Boot Args）**：
    - `-um790fanoff`：完全禁用 `UM790Fan.kext`（风扇与 CPPC 均接管回 BIOS 默认）。
    - `-um790cppcoff`：仅禁用 CPPC 写入，保留风扇控制功能。

### 2. `patches/nvmtl-um790pro-optimizations.patch`：`NVMTLDriver` Metal→Vulkan 转译与显存调度优化
基于 NullMoth / `nullarc` (`a209038`) 针对 Radeon 780M APU 与 macOS 桌面合成特征的补丁，覆盖以下 5 项核心改进：
1. **消除 AMD APU `res2` 显存抖动与分配线程卡顿**：
   - 修复独显默认的 `1024 MiB` headroom + `256 MiB` margin（`1356 MiB` target）与 AMD `128 MiB` 软上限冲突引发的 DDR5↔DDR5 纹理频繁迁出/迁回（此前 10 分钟内高达 30.8 GB 搬运与 16–30 ms 主线程阻塞）。
   - AMD (`0x1002`) 下默认关闭 `res2` LRU 页面搬运（保留 `res1` purgeable 显存回收，可用 `NVMTL_NO_RES2=0` 随时手动开启）；将 `WindowServer` 默认 `nvmtl_amd_vram_cap()` 提升至 `256 MiB`（其他进程维持 `128 MiB`），`headroom` 按容量自适应为 `8–32 MiB`。
2. **持久化并跨进程共享 `VkPipelineCache`**：
   - 缓存路径从重启即清空的 `TMPDIR` (`/T/`) 迁移到持久化的用户缓存目录 `.../C/nvmtl/`，落盘阈值从 ≥ 8 条管线降至 ≥ 2 条。
   - 引入同目录共享种子 `nvmtl-pcache-<uid>-<vendor>-shared.bin` 与 `vkMergePipelineCaches` 原子合并写回，消除“系统设置”等短生命周期 `.appex` 子面板冷启动从零编译卡顿。
3. **`nvmtl_surface_keep` 节流与容量限制**：
   - 增加 2000 ms 空闲 TTL、48 条目 / 192 MiB LRU 上限及 `IOSurfaceGetBaseAddress` 命中一致性校验。
   - 增加 250 ms 时间节流，消除活跃 IOSurface > 48 个时每次创建纹理都触发全字典 $O(N)$ 扫描的瓶颈。
4. **不可变采样器缓存与日志限流**：
   - 在 `newSamplerStateWithDescriptor:` 为无 `label` 的不可变采样器增加 256 槽线程安全缓存，并复用静态 `dispatch_once` 字符串数组、在展开 `.description.UTF8String` 前进行日志限流。
5. **非 256B 对齐线性纹理快速失败与格式查询缓存**：
   - 在 `nvmtl_vk_image_create_buffer_alias_lifetime_body` 中对 AMD 非 256B 对齐行距的线性纹理直接静默快速失败，并原子缓存 Vulkan 格式属性查询函数指针。
6. **过滤 AIR 空内联汇编内存屏障（修复 Chromium / Electron UI 渐变文字隐身）**：
   - 在转译前通过 `nvmtl_air_strip_noop_asm()` 安全剔除 LLVM 生成的空内联汇编屏障 `[tail|musttail|notail] call void asm sideeffect "", ""()`（`libnvmtl_translate` 会因 `opcode=tail` / `opcode=call` 拒绝编译导致 Skia 文本渐变着色器缺失、`draw with no pipeline set`），实测修复 `aircache` 中 **30 / 30** 失败的 `@main0` 着色器。

### 3. `um790-bench/`：零重启稳定性、CPU 与 Metal→Vulkan 压测审计套件
- 原生 Metal/CPU 压测核心 [`src/metal_stress.m`](um790-bench/src/metal_stress.m) + 编排 CLI [`um790bench.py`](um790-bench/um790bench.py)。
- 支持 `run`（全项基准 + `sysctl` 内核计数器审计 + Crash 监控）、`ab`（环境变量 A/B 对比）、`audit` 与 `compare`。

---

## 实测性能对比（UM790 Pro 实机数据）

### `rdtsc` 纯整型单核 / 多核吞吐对比（CPPC 解锁前后）
| 线程数 | 开启 CPPC 前 | 开启 CPPC 后 (`balanced EPP=0x20`) | 提升幅度 |
| :--- | ---: | ---: | :--- |
| **1 线程** | `165.7 Mops/s` (`TSC_cyc/op 24.09`, ~1.65 GHz) | **`461.6 Mops/s`** (`TSC_cyc/op 8.65`, ~4.8–5.1 GHz) | **+178.6%（2.79×）** |
| **8 线程** | `3441.4 Mops/s` (`TSC_cyc/op 9.28`) | **`3747.1 Mops/s`** (`TSC_cyc/op 8.52`) | **+8.9%** |
| **16 线程** | `5790.7 Mops/s` (`TSC_cyc/op 11.03`) | **`5968.7 Mops/s`** (`TSC_cyc/op 10.69`) | **+3.1%** |

### `um790bench` 综合压测对比（原始结果见 `um790-bench/results/`）
| 指标 | 基线 (`baseline`) | 开启 CPPC 后 (`after-cppc`) | + `res2`/`pcache` 优化后 (`after-res2-pcache`) |
| :--- | ---: | ---: | ---: |
| `cpu.score_1t` | `150.70` | **`351.60` (+133.3%)** | — |
| `vram_stress.wall_ms` (320 MiB + 24 条管线) | `3088.96 ms` | **`286.04 ms` (-90.7%)** | **`283.07 ms` (-90.8%，比 `pre-res2-pcache` 的 `333.99 ms` 再降 15.2%)** |
| `composite.cpu_pct` | `35.7%` | `31.5%` | **`29.8%`** |
| 稳定性审计 (`hang / timeout / iop_fail / new_crashes`) | `0 / 0 / 0 / 0` | `0 / 0 / 0 / 0` | **`0 / 0 / 0 / 0`** |

### `WindowServer` / `NVMTLDriver` 运行日志改善
- `-1000158000` 线性缓冲纹理创建报错：`98,304 → 0`
- `pixel sampler` 冗余日志：`159,744 → 6`
- `newSamplerStateWithDescriptor` 冗余创建：`159,744 → 12`
- AMD APU `res2` 无效纹理迁出/迁回：从 10 分钟内 `~30.8 GB` 降至默认关闭 (`0`)

---

## 编译、安装与回滚指引

### 1. 编译与安装 `um790-fan`
需要 `MacKernelSDK`（可通过参数或环境变量 `MACKERNELSDK` 指定路径）：
```bash
# 编译 Kext
cd um790-fan/kext
bash build.sh /path/to/MacKernelSDK

# 编译 CLI
clang -O2 -Wall -Wextra -framework IOKit -framework CoreFoundation \
  um790-fan/cli/um790fan.c -o um790fan

# 编译菜单栏 App
cd um790-fan/app
bash build.sh
```
安装内核扩展到 `/Library/Extensions/UM790Fan.kext` 前，**务必先备份当前的 AuxKC**：
```bash
sudo cp /Library/KernelCollections/AuxiliaryKernelExtensions.kc /Users/Shared/gpu/backup/aux.kc.$(date +%Y%m%d-%H%M%S)
```
再使用 `kmutil create -n aux ...` 重建 AuxKC 并重启生效。

### 2. 应用 `NVMTLDriver` 补丁
```bash
cd /path/to/nullarc
git apply /path/to/um790-pro-macos-opt/patches/nvmtl-um790pro-optimizations.patch

# 运行单元测试
PYTHONPATH=tests python3 -m unittest \
  tests/test_native_p1_perf.py \
  tests/test_render_encoding_failure.py \
  tests/test_render_load_failure.py \
  tests/test_format_view_clear.py \
  tests/test_render_surface_clear.py

# 编译并签名 NVMTLDriver.new 后，先备份旧驱动再原子替换安装
cd src/nmplugin && bash build.sh && codesign -f -s - NVMTLDriver.new
```
> **关于 `/Library/GPUBundles/nvmtl-allow.txt`**：建议保持 `-iconservicesagent` 与 `-iconservicesd` 在黑名单中，使系统图标生成直接回退至原生 CoreGraphics SIMD CPU 光栅化，避免拉起整套 Vulkan/RADV 实例。

### 3. 运行 `um790-bench` 压测
```bash
cd um790-bench
mkdir -p build
clang -O2 -Wall -Wextra -framework Metal -framework Foundation -framework QuartzCore -framework IOKit \
  src/metal_stress.m -o build/metal_stress

./um790bench.py run --tag my-test
./um790bench.py compare results/<old>.json results/<new>.json
```
