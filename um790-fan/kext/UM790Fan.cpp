// UM790Fan.cpp — Native IOKit EC fan monitor & controller for Minisforum UM790 Pro (F7BSC, IT5571).
// Ported from minisforum_ec / FanControl.MinisforumUM (F7BSC profile) with fail-closed admission,
// allowlisted I2EC reads/writes, step limiting, anti-hunting detection, thermal limits, and
// sleep/shutdown firmware handoff.

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <architecture/i386/pio.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#include "UM790FanABI.h"

extern "C" {
int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);
void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);
}

namespace {

static constexpr uint32_t MSR_K7_HWCR         = 0xC0010015u;
static constexpr uint32_t MSR_AMD_PSTATE_STAT = 0xC0010063u;
static constexpr uint32_t MSR_AMD_CPPC_CAP1   = 0xC00102B0u;
static constexpr uint32_t MSR_AMD_CPPC_ENABLE = 0xC00102B1u;
static constexpr uint32_t MSR_AMD_CPPC_REQ    = 0xC00102B3u;

static inline void cpuidLeaf(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__("cpuid"
                         : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                         : "a"(leaf), "c"(0));
}

static inline bool safeRdmsr(uint32_t msr, uint64_t *val) {
    uint32_t lo = 0, hi = 0;
    if (rdmsr_carefully(msr, &lo, &hi) != 0)
        return false;
    *val = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
}

static inline void rawWrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = static_cast<uint32_t>(val & 0xffffffffu);
    uint32_t hi = static_cast<uint32_t>(val >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

struct CppcRendezvousCtx {
    uint8_t  epp;
    uint8_t  useLowNonlin;
    uint32_t errors;
};

static void cppcApplyOnCpu(void *arg) {
    CppcRendezvousCtx *ctx = static_cast<CppcRendezvousCtx *>(arg);
    uint64_t cap1 = 0, en = 0, reqCur = 0;
    if (!safeRdmsr(MSR_AMD_CPPC_CAP1, &cap1) ||
        !safeRdmsr(MSR_AMD_CPPC_ENABLE, &en) ||
        !safeRdmsr(MSR_AMD_CPPC_REQ, &reqCur)) {
        __atomic_fetch_add(&ctx->errors, 1u, __ATOMIC_RELAXED);
        return;
    }
    uint8_t lowest    = static_cast<uint8_t>(cap1 & 0xffu);
    uint8_t lowNonlin = static_cast<uint8_t>((cap1 >> 8) & 0xffu);
    uint8_t highest   = static_cast<uint8_t>((cap1 >> 24) & 0xffu);
    if (highest == 0 || highest <= lowest) {
        __atomic_fetch_add(&ctx->errors, 1u, __ATOMIC_RELAXED);
        return;
    }
    if ((en & 1ULL) == 0)
        rawWrmsr(MSR_AMD_CPPC_ENABLE, 1ULL);
    uint8_t minP = ctx->useLowNonlin ? lowNonlin : lowest;
    if (minP == 0 || minP > highest)
        minP = lowest;
    uint64_t req = static_cast<uint64_t>(highest)
                 | (static_cast<uint64_t>(minP) << 8)
                 | (0ULL << 16)
                 | (static_cast<uint64_t>(ctx->epp) << 24);
    rawWrmsr(MSR_AMD_CPPC_REQ, req);
}

static constexpr uint16_t SIO_INDEX        = 0x2e;
static constexpr uint16_t SIO_DATA         = 0x2f;
static constexpr uint8_t  SIO_REG_CHIP_ID  = 0x20;
static constexpr uint8_t  SIO_REG_D2_INDEX = 0x2e;
static constexpr uint8_t  SIO_REG_D2_DATA  = 0x2f;

static constexpr uint8_t  D2_I2EC_ADDR_LOW  = 0x10;
static constexpr uint8_t  D2_I2EC_ADDR_HIGH = 0x11;
static constexpr uint8_t  D2_I2EC_DATA      = 0x12;

static constexpr uint16_t EC_TEMP_SYS          = 0x0305;
static constexpr uint16_t EC_TEMP_CPU          = 0x0309;
static constexpr uint16_t EC_CPU_ROWS          = 0x0310; // 8 x (base, upper, lower); row 7 is critical
static constexpr uint16_t EC_CPU_SELECTOR      = 0x032f;
static constexpr uint16_t EC_SYS_POLICY_ROWS   = 0x0330; // 3 x (code, upper, lower)
static constexpr uint16_t EC_SYS_TARGET        = 0x0885;
static constexpr uint16_t EC_SYS_EFFECTIVE     = 0x0889;
static constexpr uint16_t EC_CPU_TEMP_OVERRIDE = 0x088a;
static constexpr uint16_t EC_SYS_OVERRIDE      = 0x088b;
static constexpr uint16_t EC_CPU_SLOPES        = 0x08b0; // 8 slopes; slope 7 is critical
static constexpr uint16_t EC_SYS_POLICY_TAIL   = 0x08c0; // 3 bytes, all zero
static constexpr uint16_t EC_DUTY_CPU          = 0x1803;
static constexpr uint16_t EC_DUTY_SYS          = 0x1804;
static constexpr uint16_t EC_TACH_CPU          = 0x181e;
static constexpr uint16_t EC_TACH_SYS          = 0x1820;

static constexpr int      CPU_ROWS          = 7;
static constexpr uint8_t  SYS_SENTINEL      = 0xff;
static constexpr int      STABLE_ATTEMPTS   = 4;
static constexpr int      TACH_ATTEMPTS     = 3;
static constexpr uint32_t TACH_DIVIDEND     = 2156250u;
static constexpr int      SYS_RELEASE_STEPS = 4;
static constexpr int      SYS_HANDOFF_POLLS = 16;
static constexpr uint32_t SYS_HANDOFF_MS    = 100;
static constexpr size_t   SYS_POLICY_BYTES  = 12;

static constexpr uint64_t CACHE_MS            = 1000;
static constexpr uint32_t MONITOR_MS          = 2000;
static constexpr uint64_t STEP_MS             = 4000;
static constexpr int      STEP_MAX_CODES      = 4;
static constexpr int      STEP_GAP_CODES      = 4;
static constexpr uint8_t  SPINUP_CODE         = 20;
static constexpr uint8_t  CURVE_HYST          = 3;
static constexpr uint8_t  LIMIT_HYST          = 5;
static constexpr int      HUNT_WINDOW         = 6;
static constexpr int      HUNT_SPREAD         = 600;
static constexpr uint64_t HUNT_BACKOFF_MS     = 60ULL * 1000ULL;
static constexpr int      MAX_HUNTS           = 3;
static constexpr uint64_t HUNT_FORGET_MS      = 30ULL * 60ULL * 1000ULL;
static constexpr uint64_t SETTLE_MS           = 10ULL * 1000ULL;
static constexpr uint64_t WATCHDOG_MS         = 60ULL * 1000ULL;
static constexpr uint32_t MAX_READ_FAILURES   = 3;
static constexpr int      MAX_ENGAGE_FAILURES = 3;
static constexpr uint32_t RELEASE_RETRY_SHIFT = 7;
static constexpr uint32_t RESUME_SETTLE_MS    = 5000;
static constexpr int      STOP_RELEASE_TRIES  = 3;

static const uint16_t kIdentAddrs[6] = { 0x2000, 0x2001, 0x2002, 0x200d, 0x180c, 0x1841 };

static const struct {
    uint16_t first;
    uint16_t last;
} kReadableRanges[] = {
    { 0x0305, 0x0305 },
    { 0x0309, 0x0309 },
    { 0x0310, 0x0327 },
    { 0x032f, 0x032f },
    { 0x0330, 0x0338 },
    { 0x0885, 0x0885 },
    { 0x0889, 0x088b },
    { 0x08b0, 0x08b7 },
    { 0x08c0, 0x08c2 },
    { 0x1803, 0x1804 },
    { 0x180c, 0x180c },
    { 0x181e, 0x1821 },
    { 0x1841, 0x1841 },
    { 0x2000, 0x2002 },
    { 0x200d, 0x200d },
};

struct CpuTable {
    uint8_t selector;
    uint8_t temp_override;
    uint8_t rows[CPU_ROWS + 1][3];
    uint8_t slopes[CPU_ROWS + 1];
};

struct SysPolicy {
    uint8_t rows[3][3];
    uint8_t tail[3];
};

struct CpuProfile {
    uint8_t selector;
    const char *name;
    uint8_t bands[CPU_ROWS][2];
    uint8_t bases[CPU_ROWS];
    uint8_t slopes[CPU_ROWS];
};

static const CpuProfile kF7bscCpuProfiles[] = {
    {
        0x00, "Default",
        { { 25, 0 }, { 45, 25 }, { 54, 45 }, { 66, 54 }, { 76, 66 }, { 88, 76 }, { 93, 88 } },
        { 0, 16, 18, 21, 28, 34, 36 },
        { 0, 10, 33, 58, 60, 16, 200 }
    },
    {
        0xb1, "Balance",
        { { 25, 0 }, { 45, 25 }, { 54, 45 }, { 66, 54 }, { 76, 66 }, { 88, 76 }, { 93, 88 } },
        { 0, 16, 18, 21, 28, 34, 36 },
        { 0, 10, 33, 58, 60, 16, 200 }
    },
    {
        0xb2, "Performance",
        { { 25, 0 }, { 45, 25 }, { 54, 45 }, { 66, 54 }, { 76, 66 }, { 88, 76 }, { 93, 88 } },
        { 0, 18, 21, 28, 36, 40, 46 },
        { 0, 15, 77, 66, 40, 50, 100 }
    },
};

static const SysPolicy kF7bscSysPolicy = {
    { { 0, 25, 0 }, { 20, 83, 25 }, { 51, 100, 83 } },
    { 0, 0, 0 }
};

static const uint8_t kCpuCriticalRow[3] = { 51, 100, 93 };
static constexpr uint8_t kCpuCriticalSlope = 0;

enum SysState {
    SYS_FIRMWARE = 0,
    SYS_FIXED,
    SYS_RELEASING,
    SYS_UNSUPPORTED,
};

struct EcByte {
    uint16_t addr;
    uint8_t  val;
};

struct Telemetry {
    int32_t rpm[UM790_NR_FANS];
    uint8_t temp[UM790_NR_FANS];
    uint8_t sys_effective;
    uint8_t sys_override;
    uint8_t sys_target;
    uint8_t duty[UM790_NR_FANS];
};

struct SysRegs {
    uint8_t effective;
    uint8_t override_val;
    uint8_t target;
};

struct FanState {
    uint32_t id;
    const char *name;
    int      min_code;
    uint8_t  temp_limit;

    uint8_t  mode;
    int      manual_code;
    uint64_t deadline_ms;

    bool     owned;
    bool     release_pending;
    int      cur;
    int      level;
    bool     hot;
    uint64_t next_step_ms;
    uint64_t settle_until_ms;
    bool     holding;
    uint64_t hold_until_ms;
    int      hunts;
    uint64_t last_hunt_ms;
    int32_t  rpm_win[HUNT_WINDOW];
    int      win_n;
    int      engage_failures;
    uint32_t release_failures;
    uint64_t next_release_ms;

    struct UM790Curve curve;
};

static inline uint64_t nowMs() {
    uint64_t absTime = 0, ns = 0;
    clock_get_uptime(&absTime);
    absolutetime_to_nanoseconds(absTime, &ns);
    return ns / 1000000ULL;
}

static inline int clampInt(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

static inline int absInt(int v) {
    return v < 0 ? -v : v;
}

static inline int divRoundClosest(int num, int den) {
    return (num + den / 2) / den;
}

// macOS <architecture/i386/pio.h>: outb(port, value) — port FIRST, value SECOND.
static inline void sioWrite(uint8_t reg, uint8_t val) {
    outb(SIO_INDEX, reg);
    outb(SIO_DATA, val);
}

static inline uint8_t sioRead(uint8_t reg) {
    outb(SIO_INDEX, reg);
    return inb(SIO_DATA);
}

static inline void sioParkPnp() {
    outb(SIO_INDEX, SIO_REG_CHIP_ID);
}

static inline void sioPark() {
    sioWrite(SIO_REG_D2_INDEX, D2_I2EC_ADDR_LOW);
    sioParkPnp();
}

static inline uint8_t pnpRead(uint8_t reg) {
    uint8_t val = sioRead(reg);
    sioParkPnp();
    return val;
}

static inline void d2Write(uint8_t reg, uint8_t val) {
    sioWrite(SIO_REG_D2_INDEX, reg);
    sioWrite(SIO_REG_D2_DATA, val);
}

static inline uint8_t i2ecRead(uint16_t addr) {
    d2Write(D2_I2EC_ADDR_HIGH, static_cast<uint8_t>(addr >> 8));
    d2Write(D2_I2EC_ADDR_LOW, static_cast<uint8_t>(addr & 0xff));
    sioWrite(SIO_REG_D2_INDEX, D2_I2EC_DATA);
    uint8_t val = sioRead(SIO_REG_D2_DATA);
    sioPark();
    return val;
}

static inline void i2ecWrite(uint16_t addr, uint8_t val) {
    d2Write(D2_I2EC_ADDR_HIGH, static_cast<uint8_t>(addr >> 8));
    d2Write(D2_I2EC_ADDR_LOW, static_cast<uint8_t>(addr & 0xff));
    sioWrite(SIO_REG_D2_INDEX, D2_I2EC_DATA);
    sioWrite(SIO_REG_D2_DATA, val);
    sioPark();
}

static bool xramReadable(uint16_t addr) {
    for (size_t i = 0; i < sizeof(kReadableRanges) / sizeof(kReadableRanges[0]); i++) {
        if (addr >= kReadableRanges[i].first && addr <= kReadableRanges[i].last)
            return true;
    }
    return false;
}

static int xramRead(uint16_t addr, uint8_t *val) {
    if (!xramReadable(addr)) {
        IOLog("UM790Fan: blocked non-allowlisted EC read 0x%04x\n", addr);
        return -1;
    }
    *val = i2ecRead(addr);
    return 0;
}

static int xramReadRange(uint16_t first, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (xramRead(static_cast<uint16_t>(first + i), &out[i]) != 0)
            return -1;
    }
    return 0;
}

static int readCpuTable(void *buf) {
    CpuTable *t = static_cast<CpuTable *>(buf);
    if (xramRead(EC_CPU_SELECTOR, &t->selector) != 0) return -1;
    if (xramRead(EC_CPU_TEMP_OVERRIDE, &t->temp_override) != 0) return -1;
    if (xramReadRange(EC_CPU_ROWS, &t->rows[0][0], sizeof(t->rows)) != 0) return -1;
    return xramReadRange(EC_CPU_SLOPES, t->slopes, sizeof(t->slopes));
}

static int readSysPolicy(void *buf) {
    SysPolicy *p = static_cast<SysPolicy *>(buf);
    if (xramReadRange(EC_SYS_POLICY_ROWS, &p->rows[0][0], sizeof(p->rows)) != 0) return -1;
    return xramReadRange(EC_SYS_POLICY_TAIL, p->tail, sizeof(p->tail));
}

static int readStable(int (*fn)(void *), void *out, void *tmp, size_t sz) {
    if (fn(out) != 0) return -1;
    for (int i = 1; i < STABLE_ATTEMPTS; i++) {
        if (fn(tmp) != 0) return -1;
        if (memcmp(tmp, out, sz) == 0) return 0;
        memcpy(out, tmp, sz);
    }
    return -1;
}

static int readTach(uint16_t low, int32_t *rpm) {
    uint8_t lo1 = 0, hi = 0, lo2 = 0;
    for (int i = 0; i < TACH_ATTEMPTS; i++) {
        if (xramRead(low, &lo1) != 0 ||
            xramRead(static_cast<uint16_t>(low + 1), &hi) != 0 ||
            xramRead(low, &lo2) != 0) {
            return -1;
        }
        if (lo1 != lo2) continue;
        uint16_t counter = static_cast<uint16_t>(lo1 | (hi << 8));
        *rpm = counter ? static_cast<int32_t>(TACH_DIVIDEND / counter) : 0;
        return 0;
    }
    return -1;
}

static inline bool plausibleTemp(uint8_t val) {
    return val >= 1 && val <= 120;
}

static SysState classifySys(uint8_t effective, uint8_t override_val, uint8_t target) {
    if (target > UM790_CODE_MAX) return SYS_UNSUPPORTED;
    if (override_val == 0) {
        if (plausibleTemp(effective)) return SYS_FIRMWARE;
        return effective == SYS_SENTINEL ? SYS_RELEASING : SYS_UNSUPPORTED;
    }
    if (override_val == SYS_SENTINEL && (effective == SYS_SENTINEL || plausibleTemp(effective)))
        return SYS_FIXED;
    return SYS_UNSUPPORTED;
}

static int readSysRegs(SysRegs *r) {
    if (xramRead(EC_SYS_EFFECTIVE, &r->effective) != 0) return -1;
    if (xramRead(EC_SYS_OVERRIDE, &r->override_val) != 0) return -1;
    return xramRead(EC_SYS_TARGET, &r->target);
}

static int readTelemetry(Telemetry *t) {
    if (readTach(EC_TACH_CPU, &t->rpm[UM790_FAN_CPU]) != 0) return -1;
    if (readTach(EC_TACH_SYS, &t->rpm[UM790_FAN_SYS]) != 0) return -1;
    if (xramRead(EC_TEMP_CPU, &t->temp[UM790_FAN_CPU]) != 0) return -1;
    if (xramRead(EC_TEMP_SYS, &t->temp[UM790_FAN_SYS]) != 0) return -1;
    if (xramRead(EC_SYS_EFFECTIVE, &t->sys_effective) != 0) return -1;
    if (xramRead(EC_SYS_OVERRIDE, &t->sys_override) != 0) return -1;
    if (xramRead(EC_SYS_TARGET, &t->sys_target) != 0) return -1;
    if (xramRead(EC_DUTY_CPU, &t->duty[UM790_FAN_CPU]) != 0) return -1;
    return xramRead(EC_DUTY_SYS, &t->duty[UM790_FAN_SYS]);
}

static const CpuProfile *findCpuProfile(uint8_t selector) {
    for (size_t i = 0; i < sizeof(kF7bscCpuProfiles) / sizeof(kF7bscCpuProfiles[0]); i++) {
        if (kF7bscCpuProfiles[i].selector == selector)
            return &kF7bscCpuProfiles[i];
    }
    return nullptr;
}

static const char *checkCpuStatic(const CpuTable *t, const CpuProfile *p) {
    if (!p || t->selector != p->selector)
        return "CPU table selector is unknown or changed";
    if (t->temp_override != 0)
        return "CPU temperature override is not zero";
    for (int r = 0; r < CPU_ROWS; r++) {
        if (t->rows[r][1] != p->bands[r][0] || t->rows[r][2] != p->bands[r][1])
            return "CPU temperature bands do not match F7BSC";
    }
    if (memcmp(t->rows[CPU_ROWS], kCpuCriticalRow, sizeof(kCpuCriticalRow)) != 0 ||
        t->slopes[CPU_ROWS] != kCpuCriticalSlope) {
        return "CPU critical row is not (51, 100, 93, 0)";
    }
    return nullptr;
}

static bool cpuIsCanonical(const CpuTable *t, const CpuProfile *p) {
    for (int r = 0; r < CPU_ROWS; r++) {
        if (t->rows[r][0] != p->bases[r] || t->slopes[r] != p->slopes[r])
            return false;
    }
    return true;
}

static bool cpuIsFixed(const CpuTable *t, int code) {
    for (int r = 0; r < CPU_ROWS; r++) {
        if (t->rows[r][0] != code || t->slopes[r] != 0)
            return false;
    }
    return true;
}

static bool slopeRestorePrefix(const uint8_t *obs, const uint8_t *canon) {
    for (int start = 0; start <= CPU_ROWS; start++) {
        for (int end = start; end <= CPU_ROWS; end++) {
            int i = 0;
            for (; i < CPU_ROWS; i++) {
                if (obs[i] != ((i >= start && i < end) ? canon[i] : 0))
                    break;
            }
            if (i == CPU_ROWS) return true;
        }
    }
    return false;
}

static bool enterOrRestorePrefix(const uint8_t *obs, const uint8_t *canon) {
    for (int restored = 0; restored < CPU_ROWS; restored++) {
        if (memcmp(obs, canon, static_cast<size_t>(restored)) != 0)
            continue;
        for (int written = restored + 1; written <= CPU_ROWS; written++) {
            int i = restored;
            while (i < written && obs[i] == obs[restored]) i++;
            if (i == written &&
                memcmp(obs + written, canon + written, static_cast<size_t>(CPU_ROWS - written)) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool updateOrRestorePrefix(const uint8_t *obs, const uint8_t *canon) {
    for (int restored = 0; restored < CPU_ROWS; restored++) {
        if (memcmp(obs, canon, static_cast<size_t>(restored)) != 0)
            continue;
        int t = restored;
        while (t < CPU_ROWS && obs[t] == obs[restored]) t++;
        if (t == CPU_ROWS) return true;
        int i = t;
        while (i < CPU_ROWS && obs[i] == obs[t]) i++;
        if (i == CPU_ROWS) return true;
    }
    return false;
}

static bool cpuRecoverable(const CpuTable *t, const CpuProfile *p) {
    uint8_t bases[CPU_ROWS], slopes[CPU_ROWS];
    for (int r = 0; r < CPU_ROWS; r++) {
        bases[r] = t->rows[r][0];
        slopes[r] = t->slopes[r];
    }
    if (memcmp(bases, p->bases, CPU_ROWS) == 0)
        return slopeRestorePrefix(slopes, p->slopes);
    for (int r = 0; r < CPU_ROWS; r++) {
        if (slopes[r] != 0 || bases[r] > UM790_CODE_MAX)
            return false;
    }
    return enterOrRestorePrefix(bases, p->bases) || updateOrRestorePrefix(bases, p->bases);
}

static size_t sysPolicyExpectations(EcByte *out) {
    const uint8_t *rows = &kF7bscSysPolicy.rows[0][0];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(kF7bscSysPolicy.rows); i++)
        out[n++] = { static_cast<uint16_t>(EC_SYS_POLICY_ROWS + i), rows[i] };
    for (size_t i = 0; i < sizeof(kF7bscSysPolicy.tail); i++)
        out[n++] = { static_cast<uint16_t>(EC_SYS_POLICY_TAIL + i), kF7bscSysPolicy.tail[i] };
    return n;
}

static size_t cpuExpectations(const CpuTable *t, EcByte *out) {
    const uint8_t *rows = &t->rows[0][0];
    size_t n = 0;
    out[n++] = { EC_CPU_SELECTOR, t->selector };
    out[n++] = { EC_CPU_TEMP_OVERRIDE, t->temp_override };
    for (size_t i = 0; i < sizeof(t->rows); i++)
        out[n++] = { static_cast<uint16_t>(EC_CPU_ROWS + i), rows[i] };
    for (size_t i = 0; i < sizeof(t->slopes); i++)
        out[n++] = { static_cast<uint16_t>(EC_CPU_SLOPES + i), t->slopes[i] };
    return n;
}

static bool codeValidForFan(uint32_t fan_id, int code) {
    int min_code = (fan_id == UM790_FAN_CPU) ? UM790_CPU_MIN_CODE : UM790_SYS_MIN_CODE;
    return code == 0 || (code >= min_code && code <= UM790_CODE_MAX);
}

static bool curveIsValid(uint32_t fan_id, const struct UM790Curve *c) {
    if (!c || c->n == 0 || c->n > UM790_CURVE_MAX_POINTS)
        return false;
    for (uint8_t i = 0; i < c->n; i++) {
        if (c->temp[i] > 120)
            return false;
        if (i > 0 && c->temp[i] <= c->temp[i - 1])
            return false;
        if (c->code[i] != UM790_CURVE_FIRMWARE && !codeValidForFan(fan_id, c->code[i]))
            return false;
    }
    return true;
}

} // namespace

class UM790Fan : public IOService {
    OSDeclareDefaultStructors(UM790Fan)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;

    IOReturn handleGetStatus(struct UM790FanStatus *out);
    IOReturn handleSetMode(const struct UM790SetModeReq *req);
    IOReturn handleSetCurve(const struct UM790SetCurveReq *req);
    IOReturn handlePingManual(const struct UM790PingReq *req);
    IOReturn handleGetCpuStatus(struct UM790CpuStatus *out);
    IOReturn handleSetCpuCppc(const struct UM790SetCpuReq *req);

private:
    IOWorkLoop         *workLoop{nullptr};
    IOCommandGate      *gate{nullptr};
    IOTimerEventSource *timer{nullptr};
    IONotifier         *pmNotifier{nullptr};

    bool disabled{false};
    bool stopped{false};
    bool suspended{false};
    bool admitted{false};
    bool ecIdentOk{false};

    uint8_t pnpBytes[3]{};
    uint8_t identBytes[6]{};
    const CpuProfile *cpuProfile{nullptr};
    char admitError[96]{};

    Telemetry lastTelemetry{};
    uint64_t  lastTelemetryMs{0};
    bool      telemetryValid{false};
    uint32_t  readFailures{0};
    uint32_t  presetId{UM790_PRESET_FIRMWARE};

    FanState fans[UM790_NR_FANS]{};

    bool     cppcSupported{false};
    bool     cppcEnabled{false};
    uint32_t cppcProfileId{UM790_CPU_PROFILE_BALANCED};
    uint8_t  cppcEpp{0x20};
    bool     cppcUseLowNonlin{false};
    uint32_t cppcApplyErrors{0};
    char     cppcMsg[96]{};

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn powerHandler(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument, vm_size_t argSize);

    static IOReturn gatedGetStatus(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedSetMode(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedSetCurve(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedPingManual(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedGetCpuStatus(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedSetCpuCppc(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedPower(OSObject *owner, void *a0, void *, void *, void *);
    static IOReturn gatedStop(OSObject *owner, void *, void *, void *, void *);

    void initCppc();
    void applyCppcAllCpus();

    void initFans();
    bool checkController(bool logErr);
    bool xramWritable(uint16_t addr, uint8_t val) const;
    int  xramWriteVerified(const EcByte *before, size_t nBefore,
                           const EcByte *writes, size_t nWrites, bool *issued);
    int  checkSysPolicy(bool logErr);

    int  sysWaitEffective(bool wantOwned);
    int  sysReleaseCore();
    int  sysEngage(int *startCode, bool *issued);
    int  sysSet(uint8_t code, int curCode);
    bool sysHeld(const Telemetry &t, int curCode) const;

    int  cpuEngage(int rpm, int *startCode, bool *issued);
    int  cpuSet(uint8_t code, int curCode);
    int  cpuRelease();
    bool cpuHeld(int curCode) const;

    int  fanEngage(FanState &f, int rpm);
    int  fanSet(FanState &f, int code);
    int  fanRelease(FanState &f, const char *reason);
    int  fanNextCode(const FanState &f, int goal) const;
    int  curveGoal(FanState &f, uint8_t temp) const;
    void fanCheckHunting(FanState &f, int rpm, uint64_t now);
    void fanTick(FanState &f, const Telemetry &t, uint64_t now);
    void monitorTick();
    void releaseAllBlocking(const char *reason);
    void admitHardware();
};

class UM790FanUserClient : public IOUserClient {
    OSDeclareDefaultStructors(UM790FanUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;

private:
    UM790Fan *owner{nullptr};
};

OSDefineMetaClassAndStructors(UM790Fan, IOService)
OSDefineMetaClassAndStructors(UM790FanUserClient, IOUserClient)

void UM790Fan::initFans() {
    memset(fans, 0, sizeof(fans));

    fans[UM790_FAN_CPU].id         = UM790_FAN_CPU;
    fans[UM790_FAN_CPU].name       = "cpu";
    fans[UM790_FAN_CPU].min_code   = UM790_CPU_MIN_CODE;
    fans[UM790_FAN_CPU].temp_limit = UM790_CPU_TEMP_LIMIT;
    fans[UM790_FAN_CPU].mode       = UM790_MODE_FIRMWARE;
    fans[UM790_FAN_CPU].cur        = -1;
    fans[UM790_FAN_CPU].manual_code = 18;

    fans[UM790_FAN_SYS].id         = UM790_FAN_SYS;
    fans[UM790_FAN_SYS].name       = "system";
    fans[UM790_FAN_SYS].min_code   = UM790_SYS_MIN_CODE;
    fans[UM790_FAN_SYS].temp_limit = UM790_SYS_TEMP_LIMIT;
    fans[UM790_FAN_SYS].mode       = UM790_MODE_FIRMWARE;
    fans[UM790_FAN_SYS].cur        = -1;
    fans[UM790_FAN_SYS].manual_code = 16;

    um790_fill_preset_curves(UM790_PRESET_SILENT,
                             &fans[UM790_FAN_CPU].curve,
                             &fans[UM790_FAN_SYS].curve);
}

bool UM790Fan::checkController(bool logErr) {
    for (size_t i = 0; i < 3; i++)
        pnpBytes[i] = pnpRead(static_cast<uint8_t>(SIO_REG_CHIP_ID + i));
    if (pnpBytes[0] != 0x55 || pnpBytes[1] != 0x71) {
        if (logErr) {
            snprintf(admitError, sizeof(admitError),
                     "Super-I/O chip ID %02x %02x %02x is not IT5571",
                     pnpBytes[0], pnpBytes[1], pnpBytes[2]);
            IOLog("UM790Fan: %s\n", admitError);
        }
        return false;
    }

    uint8_t expected[6] = { 0x55, 0x71, pnpBytes[2], 0x43, 0x14, 0x7f };
    for (size_t i = 0; i < 6; i++) {
        if (xramRead(kIdentAddrs[i], &identBytes[i]) != 0)
            return false;
    }
    if (memcmp(identBytes, expected, sizeof(expected)) != 0) {
        if (logErr) {
            snprintf(admitError, sizeof(admitError),
                     "EC signature %02x %02x %02x %02x %02x %02x mismatch",
                     identBytes[0], identBytes[1], identBytes[2],
                     identBytes[3], identBytes[4], identBytes[5]);
            IOLog("UM790Fan: %s\n", admitError);
        }
        return false;
    }
    ecIdentOk = true;
    return true;
}

bool UM790Fan::xramWritable(uint16_t addr, uint8_t val) const {
    if (addr == EC_SYS_TARGET)
        return val <= UM790_CODE_MAX;
    if (addr == EC_SYS_OVERRIDE)
        return val == 0 || val == SYS_SENTINEL;
    for (int r = 0; r < CPU_ROWS; r++) {
        if (addr == EC_CPU_ROWS + 3 * r)
            return val <= UM790_CODE_MAX;
        if (addr == EC_CPU_SLOPES + r)
            return val == 0 || (cpuProfile && val == cpuProfile->slopes[r]);
    }
    return false;
}

int UM790Fan::xramWriteVerified(const EcByte *before, size_t nBefore,
                                const EcByte *writes, size_t nWrites, bool *issued) {
    for (size_t i = 0; i < nWrites; i++) {
        if (!xramWritable(writes[i].addr, writes[i].val)) {
            IOLog("UM790Fan: blocked EC write 0x%04x=0x%02x\n", writes[i].addr, writes[i].val);
            return -1;
        }
    }
    if (!checkController(false))
        return -1;

    uint8_t val = 0;
    for (size_t i = 0; i < nBefore; i++) {
        if (xramRead(before[i].addr, &val) != 0)
            return -1;
        if (val != before[i].val) {
            IOLog("UM790Fan: EC precondition failed at 0x%04x: expected 0x%02x, read 0x%02x\n",
                  before[i].addr, before[i].val, val);
            return -1;
        }
    }

    if (issued) *issued = true;
    for (size_t i = 0; i < nWrites; i++) {
        i2ecWrite(writes[i].addr, writes[i].val);
        if (xramRead(writes[i].addr, &val) != 0)
            return -1;
        if (val != writes[i].val) {
            IOLog("UM790Fan: EC write verification failed at 0x%04x: wrote 0x%02x, read 0x%02x\n",
                  writes[i].addr, writes[i].val, val);
            return -1;
        }
    }
    return 0;
}

int UM790Fan::checkSysPolicy(bool logErr) {
    SysPolicy pol{}, tmp{};
    if (readStable(readSysPolicy, &pol, &tmp, sizeof(pol)) != 0) {
        if (logErr) {
            snprintf(admitError, sizeof(admitError), "system fan policy not stable");
            IOLog("UM790Fan: %s\n", admitError);
        }
        return -1;
    }
    if (memcmp(&pol, &kF7bscSysPolicy, sizeof(pol)) != 0) {
        if (logErr) {
            snprintf(admitError, sizeof(admitError),
                     "system fan policy mismatch (%02x %02x %02x / %02x %02x %02x)",
                     pol.rows[0][0], pol.rows[1][0], pol.rows[2][0],
                     pol.tail[0], pol.tail[1], pol.tail[2]);
            IOLog("UM790Fan: %s\n", admitError);
        }
        return -1;
    }
    return 0;
}

int UM790Fan::sysWaitEffective(bool wantOwned) {
    uint8_t last = 0;
    for (int i = 0; i < SYS_HANDOFF_POLLS; i++) {
        if (xramRead(EC_SYS_EFFECTIVE, &last) != 0)
            return -1;
        if (wantOwned ? (last == SYS_SENTINEL) : plausibleTemp(last))
            return 0;
        if (i + 1 < SYS_HANDOFF_POLLS)
            IOSleep(SYS_HANDOFF_MS);
    }
    IOLog("UM790Fan: system fan did not %s (effective=0x%02x)\n",
          wantOwned ? "enter fixed-target" : "return to firmware", last);
    return -1;
}

int UM790Fan::sysReleaseCore() {
    EcByte before[2]{}, write{};
    SysRegs r{}, failed{};
    bool writeFailed = false;

    for (int step = 0; step < SYS_RELEASE_STEPS; step++) {
        if (readSysRegs(&r) != 0) return -1;
        if (writeFailed && r.override_val == failed.override_val && r.target == failed.target)
            return -1;
        writeFailed = false;

        switch (classifySys(r.effective, r.override_val, r.target)) {
        case SYS_FIRMWARE:
            return 0;
        case SYS_RELEASING:
            if (sysWaitEffective(false) != 0) return -1;
            if (readSysRegs(&r) != 0) return -1;
            if (classifySys(r.effective, r.override_val, r.target) != SYS_FIRMWARE) {
                IOLog("UM790Fan: firmware did not resume system fan control\n");
                return -1;
            }
            return 0;
        case SYS_FIXED:
            before[0] = { EC_SYS_OVERRIDE, SYS_SENTINEL };
            before[1] = { EC_SYS_TARGET, r.target };
            if (r.target == UM790_CODE_MAX)
                write = { EC_SYS_OVERRIDE, 0 };
            else
                write = { EC_SYS_TARGET, UM790_CODE_MAX };
            if (xramWriteVerified(before, 2, &write, 1, nullptr) != 0) {
                writeFailed = true;
                failed = r;
            }
            break;
        default:
            IOLog("UM790Fan: system fan state unsupported (eff=0x%02x ovr=0x%02x tgt=%u)\n",
                  r.effective, r.override_val, r.target);
            return -1;
        }
    }
    IOLog("UM790Fan: system fan release did not reach firmware state\n");
    return -1;
}

int UM790Fan::sysEngage(int *startCode, bool *issued) {
    if (checkSysPolicy(false) != 0) return -1;

    EcByte before[SYS_POLICY_BYTES + 3]{};
    EcByte write{ EC_SYS_OVERRIDE, SYS_SENTINEL };
    SysRegs r{};
    int ret = -1;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (readSysRegs(&r) != 0) return -1;
        if (classifySys(r.effective, r.override_val, r.target) != SYS_FIRMWARE) {
            IOLog("UM790Fan: sysEngage state not firmware (eff=0x%02x ovr=0x%02x tgt=%u)\n",
                  r.effective, r.override_val, r.target);
            return -1;
        }
        size_t n = sysPolicyExpectations(before);
        before[n++] = { EC_SYS_EFFECTIVE, r.effective };
        before[n++] = { EC_SYS_OVERRIDE, r.override_val };
        before[n++] = { EC_SYS_TARGET, r.target };
        ret = xramWriteVerified(before, n, &write, 1, issued);
        if (ret == 0 || *issued) break;
    }
    if (ret != 0) return ret;
    if (sysWaitEffective(true) != 0) return -1;
    uint8_t target = 0;
    if (xramRead(EC_SYS_TARGET, &target) != 0) return -1;
    if (target > UM790_CODE_MAX) {
        IOLog("UM790Fan: invalid system fan target %u after handoff\n", target);
        return -1;
    }
    *startCode = target;
    return 0;
}

int UM790Fan::sysSet(uint8_t code, int curCode) {
    if (checkSysPolicy(false) != 0) return -1;
    EcByte before[SYS_POLICY_BYTES + 3]{};
    EcByte write{ EC_SYS_TARGET, code };
    size_t n = sysPolicyExpectations(before);
    before[n++] = { EC_SYS_EFFECTIVE, SYS_SENTINEL };
    before[n++] = { EC_SYS_OVERRIDE, SYS_SENTINEL };
    before[n++] = { EC_SYS_TARGET, static_cast<uint8_t>(curCode) };
    if (xramWriteVerified(before, n, &write, 1, nullptr) != 0) return -1;
    SysRegs r{};
    if (readSysRegs(&r) != 0) return -1;
    if (r.effective != SYS_SENTINEL || r.override_val != SYS_SENTINEL || r.target != code) {
        IOLog("UM790Fan: sysSet verification mismatch after writing %u\n", code);
        return -1;
    }
    return 0;
}

bool UM790Fan::sysHeld(const Telemetry &t, int curCode) const {
    return t.sys_effective == SYS_SENTINEL &&
           t.sys_override == SYS_SENTINEL &&
           t.sys_target == curCode;
}

int UM790Fan::cpuEngage(int rpm, int *startCode, bool *issued) {
    int code = clampInt(divRoundClosest(rpm, 100), 0, UM790_CODE_MAX);
    if (code > 0 && code < UM790_CPU_MIN_CODE)
        code = UM790_CPU_MIN_CODE;

    CpuTable t{}, tmp{};
    if (readStable(readCpuTable, &t, &tmp, sizeof(t)) != 0) return -1;
    const char *problem = checkCpuStatic(&t, cpuProfile);
    if (!problem && !cpuIsCanonical(&t, cpuProfile))
        problem = "CPU table is not canonical firmware table";
    if (problem) {
        IOLog("UM790Fan: cpuEngage refused: %s\n", problem);
        return -1;
    }

    EcByte before[2 + sizeof(t.rows) + sizeof(t.slopes)]{};
    EcByte writes[2 * CPU_ROWS]{};
    size_t nw = 0;
    for (int r = 0; r < CPU_ROWS; r++)
        writes[nw++] = { static_cast<uint16_t>(EC_CPU_SLOPES + r), 0 };
    for (int r = 0; r < CPU_ROWS; r++)
        writes[nw++] = { static_cast<uint16_t>(EC_CPU_ROWS + 3 * r), static_cast<uint8_t>(code) };

    if (xramWriteVerified(before, cpuExpectations(&t, before), writes, nw, issued) != 0)
        return -1;
    *startCode = code;
    return 0;
}

int UM790Fan::cpuSet(uint8_t code, int curCode) {
    CpuTable t{}, tmp{};
    if (readStable(readCpuTable, &t, &tmp, sizeof(t)) != 0) return -1;
    const char *problem = checkCpuStatic(&t, cpuProfile);
    if (!problem && !cpuIsFixed(&t, curCode))
        problem = "CPU table not in expected driver-held state";
    if (problem) {
        IOLog("UM790Fan: cpuSet failed: %s\n", problem);
        return -1;
    }

    EcByte before[2 + sizeof(t.rows) + sizeof(t.slopes)]{};
    EcByte writes[CPU_ROWS]{};
    for (int r = 0; r < CPU_ROWS; r++)
        writes[r] = { static_cast<uint16_t>(EC_CPU_ROWS + 3 * r), code };
    return xramWriteVerified(before, cpuExpectations(&t, before), writes, CPU_ROWS, nullptr);
}

int UM790Fan::cpuRelease() {
    if (!cpuProfile) return -1;
    CpuTable t{}, tmp{};
    if (readStable(readCpuTable, &t, &tmp, sizeof(t)) != 0) return -1;
    const char *problem = checkCpuStatic(&t, cpuProfile);
    if (problem) {
        IOLog("UM790Fan: cpuRelease refused: %s\n", problem);
        return -1;
    }
    if (cpuIsCanonical(&t, cpuProfile)) return 0;
    if (!cpuRecoverable(&t, cpuProfile)) {
        IOLog("UM790Fan: CPU table unrecoverable; no restore write attempted\n");
        return -1;
    }

    EcByte before[2 + sizeof(t.rows) + sizeof(t.slopes)]{};
    EcByte writes[3 * CPU_ROWS]{};
    size_t nw = 0;
    for (int r = 0; r < CPU_ROWS; r++)
        writes[nw++] = { static_cast<uint16_t>(EC_CPU_SLOPES + r), 0 };
    for (int r = 0; r < CPU_ROWS; r++)
        writes[nw++] = { static_cast<uint16_t>(EC_CPU_ROWS + 3 * r), cpuProfile->bases[r] };
    for (int r = 0; r < CPU_ROWS; r++)
        writes[nw++] = { static_cast<uint16_t>(EC_CPU_SLOPES + r), cpuProfile->slopes[r] };

    xramWriteVerified(before, cpuExpectations(&t, before), writes, nw, nullptr);

    if (readStable(readCpuTable, &t, &tmp, sizeof(t)) != 0) return -1;
    if (checkCpuStatic(&t, cpuProfile) != nullptr || !cpuIsCanonical(&t, cpuProfile)) {
        IOLog("UM790Fan: canonical CPU table was not restored\n");
        return -1;
    }
    return 0;
}

bool UM790Fan::cpuHeld(int curCode) const {
    CpuTable t{};
    for (int attempt = 0; attempt < 2; attempt++) {
        if (readCpuTable(&t) == 0 &&
            checkCpuStatic(&t, cpuProfile) == nullptr &&
            cpuIsFixed(&t, curCode)) {
            return true;
        }
    }
    return false;
}

int UM790Fan::fanRelease(FanState &f, const char *reason) {
    if (!f.owned && !f.release_pending) return 0;
    f.release_pending = true;
    telemetryValid = false;

    int ret = (f.id == UM790_FAN_CPU) ? cpuRelease() : sysReleaseCore();
    if (ret != 0) {
        uint32_t shift = (f.release_failures < RELEASE_RETRY_SHIFT) ? f.release_failures : RELEASE_RETRY_SHIFT;
        uint64_t delay = static_cast<uint64_t>(MONITOR_MS) << shift;
        f.release_failures++;
        f.next_release_ms = nowMs() + delay;
        IOLog("UM790Fan: %s fan release failed %u times (%s); retry in %llu ms\n",
              f.name, f.release_failures, reason, delay);
        return -1;
    }
    f.release_failures = 0;
    f.owned = false;
    f.release_pending = false;
    f.cur = -1;
    f.win_n = 0;
    IOLog("UM790Fan: %s fan returned to firmware (%s)\n", f.name, reason);
    return 0;
}

int UM790Fan::fanEngage(FanState &f, int rpm) {
    bool issued = false;
    int start = 0;
    telemetryValid = false;
    int ret = (f.id == UM790_FAN_CPU) ? cpuEngage(rpm, &start, &issued) : sysEngage(&start, &issued);
    if (ret != 0) {
        if (issued) {
            f.owned = true;
            fanRelease(f, "taking control failed");
        } else {
            if (f.id == UM790_FAN_CPU) cpuRelease();
            else sysReleaseCore();
        }
        return -1;
    }
    uint64_t now = nowMs();
    f.owned = true;
    f.cur = start;
    f.next_step_ms = now + STEP_MS;
    f.settle_until_ms = now + SETTLE_MS;
    f.win_n = 0;
    IOLog("UM790Fan: %s fan under driver control (mode=%u), start=%d RPM\n",
          f.name, f.mode, start * 100);
    return 0;
}

int UM790Fan::fanSet(FanState &f, int code) {
    telemetryValid = false;
    int ret = (f.id == UM790_FAN_CPU)
        ? cpuSet(static_cast<uint8_t>(code), f.cur)
        : sysSet(static_cast<uint8_t>(code), f.cur);
    if (ret != 0) {
        fanRelease(f, "target write failed");
        return -1;
    }
    f.cur = code;
    f.settle_until_ms = nowMs() + SETTLE_MS;
    f.win_n = 0;
    return 0;
}

int UM790Fan::fanNextCode(const FanState &f, int goal) const {
    if (f.cur == 0) return SPINUP_CODE;
    int diff = absInt(goal - f.cur);
    int step = clampInt((diff + STEP_GAP_CODES - 1) / STEP_GAP_CODES, 1, STEP_MAX_CODES);
    int next = f.cur + (goal > f.cur ? step : -step);
    if (next > 0 && next < f.min_code)
        next = (goal > f.cur) ? f.min_code : 0;
    return next;
}

int UM790Fan::curveGoal(FanState &f, uint8_t temp) const {
    if (f.curve.n == 0) return UM790_CURVE_FIRMWARE;
    int level = clampInt(f.level, 0, static_cast<int>(f.curve.n) - 1);
    while (level + 1 < static_cast<int>(f.curve.n) && temp >= f.curve.temp[level + 1])
        level++;
    while (level > 0 && static_cast<int>(temp) + CURVE_HYST < static_cast<int>(f.curve.temp[level]))
        level--;
    f.level = level;
    return f.curve.code[level];
}

void UM790Fan::fanCheckHunting(FanState &f, int rpm, uint64_t now) {
    if (f.hunts && now > f.last_hunt_ms + HUNT_FORGET_MS)
        f.hunts = 0;
    if (now < f.settle_until_ms)
        return;

    f.rpm_win[f.win_n % HUNT_WINDOW] = rpm;
    if (++f.win_n < HUNT_WINDOW)
        return;

    int32_t lo = f.rpm_win[0], hi = f.rpm_win[0];
    for (int i = 1; i < HUNT_WINDOW; i++) {
        if (f.rpm_win[i] < lo) lo = f.rpm_win[i];
        if (f.rpm_win[i] > hi) hi = f.rpm_win[i];
    }
    if (hi - lo <= HUNT_SPREAD)
        return;

    f.hunts++;
    f.last_hunt_ms = now;
    IOLog("UM790Fan: %s fan hunting at %d RPM (measured %d..%d)\n",
          f.name, f.cur * 100, lo, hi);
    fanRelease(f, "hunting");
    if (f.hunts >= MAX_HUNTS) {
        f.mode = UM790_MODE_FIRMWARE;
        f.hunts = 0;
        IOLog("UM790Fan: %s fan hunted %d times; left to firmware\n", f.name, MAX_HUNTS);
    } else {
        f.holding = true;
        f.hold_until_ms = now + HUNT_BACKOFF_MS;
    }
}

void UM790Fan::fanTick(FanState &f, const Telemetry &t, uint64_t now) {
    if (!admitted) return;

    uint8_t temp = t.temp[f.id];
    int32_t rpm  = t.rpm[f.id];

    if (f.release_pending) {
        if (now >= f.next_release_ms)
            fanRelease(f, "retry");
        return;
    }

    if (!f.hot && (temp >= f.temp_limit || !plausibleTemp(temp))) {
        f.hot = true;
        IOLog("UM790Fan: %s fan temp %u C reached limit %u C; yielding to firmware\n",
              f.name, temp, f.temp_limit);
    } else if (f.hot && plausibleTemp(temp) &&
               static_cast<int>(temp) + LIMIT_HYST < static_cast<int>(f.temp_limit)) {
        f.hot = false;
        IOLog("UM790Fan: %s fan temp back to %u C\n", f.name, temp);
    }

    if (f.mode == UM790_MODE_MANUAL && now > f.deadline_ms) {
        f.mode = (f.curve.n > 0) ? UM790_MODE_CURVE : UM790_MODE_FIRMWARE;
        IOLog("UM790Fan: %s fan manual watchdog expired; returning to mode %u\n", f.name, f.mode);
    }
    if (f.mode == UM790_MODE_CURVE && f.curve.n == 0)
        f.mode = UM790_MODE_FIRMWARE;
    if (f.holding && now >= f.hold_until_ms)
        f.holding = false;

    const char *why = nullptr;
    if (f.hot)                              why = "temperature limit";
    else if (f.mode == UM790_MODE_FIRMWARE) why = "firmware mode";
    else if (f.holding)                     why = "backing off";

    int goal = UM790_CURVE_FIRMWARE;
    if (!why) {
        goal = (f.mode == UM790_MODE_MANUAL) ? f.manual_code : curveGoal(f, temp);
    }
    if (goal > 0 && goal < f.min_code)
        goal = f.min_code;

    if (goal == UM790_CURVE_FIRMWARE) {
        if (f.owned)
            fanRelease(f, why ? why : "curve");
        return;
    }

    if (!f.owned) {
        if (fanEngage(f, rpm) == 0) {
            f.engage_failures = 0;
        } else if (++f.engage_failures >= MAX_ENGAGE_FAILURES) {
            f.mode = UM790_MODE_FIRMWARE;
            f.engage_failures = 0;
            IOLog("UM790Fan: %s fan engage failed %d times; left to firmware\n",
                  f.name, MAX_ENGAGE_FAILURES);
        } else {
            f.holding = true;
            f.hold_until_ms = now + HUNT_BACKOFF_MS;
        }
        return;
    }

    bool heldOk = (f.id == UM790_FAN_CPU) ? cpuHeld(f.cur) : sysHeld(t, f.cur);
    if (!heldOk) {
        IOLog("UM790Fan: %s fan EC state changed externally; releasing\n", f.name);
        fanRelease(f, "unexpected state");
        f.holding = true;
        f.hold_until_ms = now + HUNT_BACKOFF_MS;
        return;
    }

    if (f.cur != goal) {
        if (now < f.next_step_ms) return;
        if (fanSet(f, fanNextCode(f, goal)) != 0) {
            f.holding = true;
            f.hold_until_ms = now + HUNT_BACKOFF_MS;
        } else {
            f.next_step_ms = now + STEP_MS;
        }
        return;
    }

    if (goal > 0)
        fanCheckHunting(f, rpm, now);
}

void UM790Fan::monitorTick() {
    if (stopped || suspended || disabled || !ecIdentOk)
        return;

    Telemetry t{};
    uint64_t now = nowMs();
    if (readTelemetry(&t) == 0) {
        readFailures = 0;
        lastTelemetry = t;
        lastTelemetryMs = now;
        telemetryValid = true;
        if (admitted) {
            for (uint32_t i = 0; i < UM790_NR_FANS; i++)
                fanTick(fans[i], t, now);
        }
    } else if (++readFailures >= MAX_READ_FAILURES && admitted) {
        for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
            FanState &f = fans[i];
            if (f.release_pending && now < f.next_release_ms) continue;
            if (f.owned || f.release_pending) {
                fanRelease(f, "EC telemetry unreadable");
                f.holding = true;
                f.hold_until_ms = now + HUNT_BACKOFF_MS;
            }
        }
    }

    if (timer && !stopped && !suspended)
        timer->setTimeoutMS(MONITOR_MS);
}

void UM790Fan::timerFired(OSObject *owner, IOTimerEventSource *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    if (self) self->monitorTick();
}

void UM790Fan::releaseAllBlocking(const char *reason) {
    if (!admitted || !ecIdentOk) return;
    // Release system fan first (no firmware temp fallback in fixed-target handoff), then CPU fan.
    for (int idx = UM790_FAN_SYS; idx >= 0; idx--) {
        FanState &f = fans[idx];
        if (!f.owned && !f.release_pending) continue;
        for (int attempt = 0; attempt < STOP_RELEASE_TRIES; attempt++) {
            if (fanRelease(f, reason) == 0) break;
            IOSleep(200);
        }
    }
}

void UM790Fan::admitHardware() {
    admitted = false;
    ecIdentOk = false;
    admitError[0] = '\0';

    if (!checkController(true))
        return;

    CpuTable cpu{}, cpuTmp{};
    if (readStable(readCpuTable, &cpu, &cpuTmp, sizeof(cpu)) != 0) {
        snprintf(admitError, sizeof(admitError), "CPU fan table read unstable");
        IOLog("UM790Fan: %s\n", admitError);
        return;
    }

    const CpuProfile *p = findCpuProfile(cpu.selector);
    const char *problem = checkCpuStatic(&cpu, p);
    if (problem) {
        snprintf(admitError, sizeof(admitError), "%s (sel=0x%02x)", problem, cpu.selector);
        IOLog("UM790Fan: %s\n", admitError);
        return;
    }
    bool cpuRepair = !cpuIsCanonical(&cpu, p);
    if (cpuRepair && !cpuRecoverable(&cpu, p)) {
        snprintf(admitError, sizeof(admitError), "CPU table non-canonical and unrecoverable");
        IOLog("UM790Fan: %s\n", admitError);
        return;
    }
    cpuProfile = p;

    if (checkSysPolicy(true) != 0)
        return;

    SysRegs r{};
    if (readSysRegs(&r) != 0) {
        snprintf(admitError, sizeof(admitError), "failed reading sys fan state");
        IOLog("UM790Fan: %s\n", admitError);
        return;
    }
    SysState st = classifySys(r.effective, r.override_val, r.target);
    if (st == SYS_UNSUPPORTED) {
        snprintf(admitError, sizeof(admitError),
                 "sys fan state unsupported (eff=0x%02x ovr=0x%02x tgt=%u)",
                 r.effective, r.override_val, r.target);
        IOLog("UM790Fan: %s\n", admitError);
        return;
    }

    fans[UM790_FAN_SYS].owned = (st != SYS_FIRMWARE);
    fans[UM790_FAN_CPU].owned = cpuRepair;

    if (st != SYS_FIRMWARE) {
        IOLog("UM790Fan: repairing leftover system fan state (eff=0x%02x ovr=0x%02x tgt=%u)\n",
              r.effective, r.override_val, r.target);
        if (fanRelease(fans[UM790_FAN_SYS], "leftover session") != 0 ||
            readSysRegs(&r) != 0 ||
            classifySys(r.effective, r.override_val, r.target) != SYS_FIRMWARE) {
            snprintf(admitError, sizeof(admitError), "could not restore system fan to firmware");
            IOLog("UM790Fan: %s\n", admitError);
            return;
        }
    }

    if (cpuRepair) {
        IOLog("UM790Fan: repairing leftover CPU fan table\n");
        if (fanRelease(fans[UM790_FAN_CPU], "leftover session") != 0) {
            snprintf(admitError, sizeof(admitError), "could not restore CPU fan to firmware");
            IOLog("UM790Fan: %s\n", admitError);
            return;
        }
    }

    if (checkSysPolicy(true) != 0)
        return;

    Telemetry t{};
    if (readTelemetry(&t) != 0) {
        snprintf(admitError, sizeof(admitError), "initial telemetry read failed");
        IOLog("UM790Fan: %s\n", admitError);
        return;
    }

    lastTelemetry = t;
    lastTelemetryMs = nowMs();
    telemetryValid = true;
    admitted = true;
    IOLog("UM790Fan: admitted F7BSC (IT5571 rev=0x%02x, profile=%s, cpu=%uC %dRPM, sys=%uC %dRPM)\n",
          pnpBytes[2], p->name,
          t.temp[UM790_FAN_CPU], t.rpm[UM790_FAN_CPU],
          t.temp[UM790_FAN_SYS], t.rpm[UM790_FAN_SYS]);
}

bool UM790Fan::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;

    initFans();

    uint32_t off = 0;
    if (PE_parse_boot_argn("-um790fanoff", &off, sizeof(off))) {
        disabled = true;
        snprintf(admitError, sizeof(admitError), "disabled via -um790fanoff boot-arg");
        IOLog("UM790Fan: %s\n", admitError);
        registerService();
        return true;
    }

    workLoop = IOWorkLoop::workLoop();
    gate     = IOCommandGate::commandGate(this);
    timer    = IOTimerEventSource::timerEventSource(this, timerFired);
    if (!workLoop || !gate || !timer ||
        workLoop->addEventSource(gate) != kIOReturnSuccess ||
        workLoop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("UM790Fan: failed creating workLoop/gate/timer\n");
        if (timer && workLoop) workLoop->removeEventSource(timer);
        if (gate && workLoop) workLoop->removeEventSource(gate);
        OSSafeReleaseNULL(timer);
        OSSafeReleaseNULL(gate);
        OSSafeReleaseNULL(workLoop);
        return false;
    }

    admitHardware();
    initCppc();

    IOPMrootDomain *pmRoot = getPMRootDomain();
    if (pmRoot)
        pmNotifier = pmRoot->registerInterest(gIOGeneralInterest, powerHandler, this, nullptr);

    if (ecIdentOk)
        timer->setTimeoutMS(MONITOR_MS);

    registerService();
    return true;
}

IOReturn UM790Fan::gatedStop(OSObject *owner, void *, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    if (!self) return kIOReturnError;
    self->stopped = true;
    if (self->timer) self->timer->cancelTimeout();
    self->releaseAllBlocking("driver unload");
    return kIOReturnSuccess;
}

void UM790Fan::stop(IOService *provider) {
    if (pmNotifier) {
        pmNotifier->remove();
        pmNotifier = nullptr;
    }
    if (gate) {
        gate->runAction(gatedStop);
    }
    if (workLoop) {
        if (timer) workLoop->removeEventSource(timer);
        if (gate)  workLoop->removeEventSource(gate);
    }
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(workLoop);
    IOService::stop(provider);
}

IOReturn UM790Fan::gatedPower(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    if (!self) return kIOReturnSuccess;
    UInt32 msg = *static_cast<UInt32 *>(a0);

    switch (msg) {
    case kIOMessageSystemWillSleep:
        self->suspended = true;
        if (self->timer) self->timer->cancelTimeout();
        self->releaseAllBlocking("system sleep");
        break;
    case kIOMessageSystemWillPowerOff:
    case kIOMessageSystemWillRestart:
        self->stopped = true;
        self->suspended = true;
        if (self->timer) self->timer->cancelTimeout();
        self->releaseAllBlocking("shutdown/restart");
        break;
    case kIOMessageSystemHasPoweredOn:
        if (!self->stopped && !self->disabled) {
            self->suspended = false;
            if (self->cppcSupported && self->cppcEnabled)
                self->applyCppcAllCpus();
            if (self->timer && self->ecIdentOk)
                self->timer->setTimeoutMS(RESUME_SETTLE_MS);
        }
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}

IOReturn UM790Fan::powerHandler(void *target, void *, UInt32 messageType,
                                IOService *, void *, vm_size_t) {
    UM790Fan *self = OSDynamicCast(UM790Fan, static_cast<OSObject *>(target));
    if (self && self->gate &&
        (messageType == kIOMessageSystemWillSleep ||
         messageType == kIOMessageSystemWillPowerOff ||
         messageType == kIOMessageSystemWillRestart ||
         messageType == kIOMessageSystemHasPoweredOn)) {
        self->gate->runAction(gatedPower, &messageType);
    }
    return kIOReturnSuccess;
}

IOReturn UM790Fan::gatedGetStatus(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    struct UM790FanStatus *out = static_cast<struct UM790FanStatus *>(a0);
    if (!self || !out) return kIOReturnBadArgument;

    memset(out, 0, sizeof(*out));
    out->abi_version   = UM790_FAN_ABI_VERSION;
    out->admitted      = self->admitted ? 1u : 0u;
    out->preset_id     = self->presetId;
    out->read_failures = self->readFailures;
    memcpy(out->pnp, self->pnpBytes, sizeof(out->pnp));
    memcpy(out->ident, self->identBytes, sizeof(out->ident));
    strlcpy(out->profile_name,
            self->cpuProfile ? self->cpuProfile->name : "",
            sizeof(out->profile_name));
    strlcpy(out->admit_error, self->admitError, sizeof(out->admit_error));

    if (!self->disabled && self->ecIdentOk) {
        uint64_t now = nowMs();
        if (!self->telemetryValid || now >= self->lastTelemetryMs + CACHE_MS) {
            Telemetry t{};
            if (readTelemetry(&t) == 0) {
                self->lastTelemetry = t;
                self->lastTelemetryMs = now;
                self->telemetryValid = true;
            }
        }
        if (self->telemetryValid) {
            for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
                out->temp_c[i] = self->lastTelemetry.temp[i];
                out->duty[i]   = self->lastTelemetry.duty[i];
                out->rpm[i]    = self->lastTelemetry.rpm[i];
            }
            out->sys_effective = self->lastTelemetry.sys_effective;
            out->sys_override  = self->lastTelemetry.sys_override;
            out->sys_target    = self->lastTelemetry.sys_target;
        }

        CpuTable cpu{};
        if (readCpuTable(&cpu) == 0) {
            out->cpu_selector      = cpu.selector;
            out->cpu_temp_override = cpu.temp_override;
            memcpy(out->cpu_rows, cpu.rows, sizeof(out->cpu_rows));
            memcpy(out->cpu_slopes, cpu.slopes, sizeof(out->cpu_slopes));
        }

        SysPolicy pol{};
        if (readSysPolicy(&pol) == 0) {
            memcpy(out->sys_policy_rows, pol.rows, sizeof(out->sys_policy_rows));
            memcpy(out->sys_policy_tail, pol.tail, sizeof(out->sys_policy_tail));
        }
    }

    for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
        const FanState &f = self->fans[i];
        out->mode[i]        = f.mode;
        out->owned[i]       = f.owned ? 1 : 0;
        out->hot[i]         = f.hot ? 1 : 0;
        out->holding[i]     = f.holding ? 1 : 0;
        out->cur_code[i]    = static_cast<int16_t>(f.cur);
        out->manual_code[i] = static_cast<int16_t>(f.manual_code);
        out->curve_level[i] = static_cast<uint8_t>(f.level);
        out->hunts[i]       = static_cast<uint8_t>(f.hunts);
        out->curves[i]      = f.curve;
    }
    return kIOReturnSuccess;
}

IOReturn UM790Fan::gatedSetMode(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    const struct UM790SetModeReq *req = static_cast<const struct UM790SetModeReq *>(a0);
    if (!self || !req) return kIOReturnBadArgument;
    if (self->disabled || !self->admitted) return kIOReturnNotPermitted;
    if (self->stopped || self->suspended) return kIOReturnNotReady;

    if (req->fan_id != UM790_FAN_CPU &&
        req->fan_id != UM790_FAN_SYS &&
        req->fan_id != UM790_FAN_ALL) {
        return kIOReturnBadArgument;
    }
    if (req->mode != UM790_MODE_MANUAL &&
        req->mode != UM790_MODE_CURVE &&
        req->mode != UM790_MODE_FIRMWARE) {
        return kIOReturnBadArgument;
    }
    if (req->mode == UM790_MODE_MANUAL &&
        (req->rpm < 0 || req->rpm > UM790_CODE_MAX * 100)) {
        return kIOReturnBadArgument;
    }

    uint64_t now = nowMs();
    for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
        if (req->fan_id != UM790_FAN_ALL && req->fan_id != i)
            continue;
        FanState &f = self->fans[i];

        if (req->mode == UM790_MODE_MANUAL) {
            int code = divRoundClosest(req->rpm, 100);
            if (req->rpm > 0 && code == 0) return kIOReturnBadArgument;
            if (!codeValidForFan(i, code)) return kIOReturnBadArgument;
            f.manual_code = code;
            f.deadline_ms = now + WATCHDOG_MS;
        } else if (req->mode == UM790_MODE_CURVE) {
            if (f.curve.n == 0) return kIOReturnBadArgument;
        }

        f.mode            = static_cast<uint8_t>(req->mode);
        f.holding         = false;
        f.hunts           = 0;
        f.engage_failures = 0;
        f.next_release_ms = now;
    }

    self->presetId = req->preset_id;
    self->monitorTick();
    return kIOReturnSuccess;
}

IOReturn UM790Fan::gatedSetCurve(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    const struct UM790SetCurveReq *req = static_cast<const struct UM790SetCurveReq *>(a0);
    if (!self || !req) return kIOReturnBadArgument;
    if (self->disabled || !self->admitted) return kIOReturnNotPermitted;
    if (req->fan_id >= UM790_NR_FANS) return kIOReturnBadArgument;
    if (!curveIsValid(req->fan_id, &req->curve)) return kIOReturnBadArgument;

    FanState &f = self->fans[req->fan_id];
    f.curve = req->curve;
    f.level = 0;
    self->presetId = req->preset_id;
    if (f.mode == UM790_MODE_CURVE && !self->stopped && !self->suspended)
        self->monitorTick();
    return kIOReturnSuccess;
}

IOReturn UM790Fan::gatedPingManual(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    const struct UM790PingReq *req = static_cast<const struct UM790PingReq *>(a0);
    if (!self || !req) return kIOReturnBadArgument;
    if (self->disabled || !self->admitted) return kIOReturnNotPermitted;
    if (req->fan_id != UM790_FAN_CPU &&
        req->fan_id != UM790_FAN_SYS &&
        req->fan_id != UM790_FAN_ALL) {
        return kIOReturnBadArgument;
    }
    uint64_t now = nowMs();
    for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
        if (req->fan_id != UM790_FAN_ALL && req->fan_id != i)
            continue;
        if (self->fans[i].mode == UM790_MODE_MANUAL)
            self->fans[i].deadline_ms = now + WATCHDOG_MS;
    }
    return kIOReturnSuccess;
}

void UM790Fan::initCppc() {
    cppcSupported = false;
    cppcEnabled = false;
    cppcApplyErrors = 0;
    cppcMsg[0] = '\0';

    uint32_t off = 0;
    if (PE_parse_boot_argn("-um790cppcoff", &off, sizeof(off))) {
        snprintf(cppcMsg, sizeof(cppcMsg), "disabled via -um790cppcoff");
        IOLog("UM790Fan: CPPC %s\n", cppcMsg);
        return;
    }

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuidLeaf(0, &eax, &ebx, &ecx, &edx);
    if (ebx != 0x68747541u || edx != 0x69746e65u || ecx != 0x444d4163u) {
        snprintf(cppcMsg, sizeof(cppcMsg), "not AuthenticAMD");
        return;
    }
    cpuidLeaf(1, &eax, &ebx, &ecx, &edx);
    if ((ecx >> 31) & 1u) {
        snprintf(cppcMsg, sizeof(cppcMsg), "running under hypervisor");
        return;
    }
    uint32_t family = ((eax >> 8) & 0xfu) + ((eax >> 20) & 0xffu);
    if (family != 0x19u && family != 0x1au) {
        snprintf(cppcMsg, sizeof(cppcMsg), "unsupported AMD family 0x%x", family);
        return;
    }
    cpuidLeaf(0x80000008u, &eax, &ebx, &ecx, &edx);
    if ((ebx & (1u << 27)) == 0) {
        snprintf(cppcMsg, sizeof(cppcMsg), "CPUID 0x80000008 EBX bit 27 (CPPC) = 0");
        IOLog("UM790Fan: %s\n", cppcMsg);
        return;
    }

    uint64_t cap1 = 0, enProbe = 0, reqProbe = 0, hwcr = 0;
    if (!safeRdmsr(MSR_AMD_CPPC_CAP1, &cap1) ||
        !safeRdmsr(MSR_AMD_CPPC_ENABLE, &enProbe) ||
        !safeRdmsr(MSR_AMD_CPPC_REQ, &reqProbe)) {
        snprintf(cppcMsg, sizeof(cppcMsg), "CPPC MSR read faulted");
        IOLog("UM790Fan: %s\n", cppcMsg);
        return;
    }
    uint8_t lowest    = static_cast<uint8_t>(cap1 & 0xffu);
    uint8_t lowNonlin = static_cast<uint8_t>((cap1 >> 8) & 0xffu);
    uint8_t nominal   = static_cast<uint8_t>((cap1 >> 16) & 0xffu);
    uint8_t highest   = static_cast<uint8_t>((cap1 >> 24) & 0xffu);
    if (highest < nominal || nominal == 0 || highest <= lowest) {
        snprintf(cppcMsg, sizeof(cppcMsg),
                 "implausible CAP1 (%u/%u/%u/%u)", lowest, lowNonlin, nominal, highest);
        IOLog("UM790Fan: %s\n", cppcMsg);
        return;
    }

    if (safeRdmsr(MSR_K7_HWCR, &hwcr) && ((hwcr >> 25) & 1u)) {
        IOLog("UM790Fan: warning: MSR_K7_HWCR CPB_DIS=1 (Core Performance Boost disabled in BIOS)\n");
    }

    cppcSupported = true;
    cppcProfileId = UM790_CPU_PROFILE_BALANCED;
    cppcEpp = 0x20;
    cppcUseLowNonlin = false;
    snprintf(cppcMsg, sizeof(cppcMsg), "ready");
    applyCppcAllCpus();
}

void UM790Fan::applyCppcAllCpus() {
    if (!cppcSupported) return;
    CppcRendezvousCtx ctx{};
    ctx.epp = cppcEpp;
    ctx.useLowNonlin = cppcUseLowNonlin ? 1 : 0;
    ctx.errors = 0;
    mp_rendezvous_no_intrs(cppcApplyOnCpu, &ctx);

    cppcApplyErrors = ctx.errors;
    uint64_t cap1 = 0, en = 0, req = 0;
    bool okEn = safeRdmsr(MSR_AMD_CPPC_ENABLE, &en) && (en & 1ULL);
    (void)safeRdmsr(MSR_AMD_CPPC_CAP1, &cap1);
    (void)safeRdmsr(MSR_AMD_CPPC_REQ, &req);
    cppcEnabled = okEn && (ctx.errors == 0);
    snprintf(cppcMsg, sizeof(cppcMsg),
             cppcEnabled ? "active" : "apply error (%u)", ctx.errors);
    IOLog("UM790Fan: CPPC %s profile=%u epp=0x%02x cap=%u/%u/%u/%u req=0x%llx errs=%u\n",
          cppcEnabled ? "active" : "FAILED",
          cppcProfileId, cppcEpp,
          static_cast<unsigned>(cap1 & 0xffu),
          static_cast<unsigned>((cap1 >> 8) & 0xffu),
          static_cast<unsigned>((cap1 >> 16) & 0xffu),
          static_cast<unsigned>((cap1 >> 24) & 0xffu),
          static_cast<unsigned long long>(req),
          ctx.errors);
}

IOReturn UM790Fan::gatedGetCpuStatus(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    struct UM790CpuStatus *out = static_cast<struct UM790CpuStatus *>(a0);
    if (!self || !out) return kIOReturnBadArgument;

    memset(out, 0, sizeof(*out));
    out->abi_version    = UM790_FAN_ABI_VERSION;
    out->supported      = self->cppcSupported ? 1u : 0u;
    out->profile_id     = self->cppcProfileId;
    out->epp            = self->cppcEpp;
    out->use_low_nonlin = self->cppcUseLowNonlin ? 1 : 0;
    out->apply_errors   = self->cppcApplyErrors;
    strlcpy(out->status_msg, self->cppcMsg, sizeof(out->status_msg));

    if (!self->disabled) {
        (void)safeRdmsr(MSR_AMD_CPPC_CAP1, &out->cap1_raw);
        (void)safeRdmsr(MSR_AMD_CPPC_ENABLE, &out->en_raw);
        (void)safeRdmsr(MSR_AMD_CPPC_REQ, &out->req_raw);
        (void)safeRdmsr(MSR_K7_HWCR, &out->hwcr_raw);
        (void)safeRdmsr(MSR_AMD_PSTATE_STAT, &out->pstate_stat_raw);
        out->enabled        = (out->en_raw & 1ULL) ? 1u : 0u;
        out->cap_lowest     = static_cast<uint8_t>(out->cap1_raw & 0xffu);
        out->cap_low_nonlin = static_cast<uint8_t>((out->cap1_raw >> 8) & 0xffu);
        out->cap_nominal    = static_cast<uint8_t>((out->cap1_raw >> 16) & 0xffu);
        out->cap_highest    = static_cast<uint8_t>((out->cap1_raw >> 24) & 0xffu);
        out->max_perf       = static_cast<uint8_t>(out->req_raw & 0xffu);
        out->min_perf       = static_cast<uint8_t>((out->req_raw >> 8) & 0xffu);
        if (out->enabled)
            out->epp = static_cast<uint8_t>((out->req_raw >> 24) & 0xffu);
    }
    return kIOReturnSuccess;
}

IOReturn UM790Fan::gatedSetCpuCppc(OSObject *owner, void *a0, void *, void *, void *) {
    UM790Fan *self = OSDynamicCast(UM790Fan, owner);
    const struct UM790SetCpuReq *req = static_cast<const struct UM790SetCpuReq *>(a0);
    if (!self || !req) return kIOReturnBadArgument;
    if (self->disabled || !self->cppcSupported) return kIOReturnNotPermitted;
    if (self->stopped || self->suspended) return kIOReturnNotReady;

    switch (req->profile_id) {
    case UM790_CPU_PROFILE_PERF:
        self->cppcProfileId = UM790_CPU_PROFILE_PERF;
        self->cppcEpp = 0x00;
        self->cppcUseLowNonlin = true;
        break;
    case UM790_CPU_PROFILE_BALANCED:
        self->cppcProfileId = UM790_CPU_PROFILE_BALANCED;
        self->cppcEpp = 0x20;
        self->cppcUseLowNonlin = false;
        break;
    case UM790_CPU_PROFILE_POWERSAVE:
        self->cppcProfileId = UM790_CPU_PROFILE_POWERSAVE;
        self->cppcEpp = 0xA0;
        self->cppcUseLowNonlin = false;
        break;
    case UM790_CPU_PROFILE_CUSTOM:
        self->cppcProfileId = UM790_CPU_PROFILE_CUSTOM;
        self->cppcEpp = req->epp;
        self->cppcUseLowNonlin = (req->use_low_nonlin != 0);
        break;
    default:
        return kIOReturnBadArgument;
    }

    self->applyCppcAllCpus();
    return (self->cppcApplyErrors == 0) ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn UM790Fan::handleGetStatus(struct UM790FanStatus *out) {
    if (!gate) {
        memset(out, 0, sizeof(*out));
        out->abi_version = UM790_FAN_ABI_VERSION;
        strlcpy(out->admit_error, admitError, sizeof(out->admit_error));
        return kIOReturnSuccess;
    }
    return gate->runAction(gatedGetStatus, out);
}

IOReturn UM790Fan::handleSetMode(const struct UM790SetModeReq *req) {
    if (!gate) return kIOReturnNotPermitted;
    return gate->runAction(gatedSetMode, const_cast<struct UM790SetModeReq *>(req));
}

IOReturn UM790Fan::handleSetCurve(const struct UM790SetCurveReq *req) {
    if (!gate) return kIOReturnNotPermitted;
    return gate->runAction(gatedSetCurve, const_cast<struct UM790SetCurveReq *>(req));
}

IOReturn UM790Fan::handlePingManual(const struct UM790PingReq *req) {
    if (!gate) return kIOReturnNotPermitted;
    return gate->runAction(gatedPingManual, const_cast<struct UM790PingReq *>(req));
}

IOReturn UM790Fan::handleGetCpuStatus(struct UM790CpuStatus *out) {
    if (!gate) {
        memset(out, 0, sizeof(*out));
        out->abi_version = UM790_FAN_ABI_VERSION;
        strlcpy(out->status_msg, cppcMsg, sizeof(out->status_msg));
        return kIOReturnSuccess;
    }
    return gate->runAction(gatedGetCpuStatus, out);
}

IOReturn UM790Fan::handleSetCpuCppc(const struct UM790SetCpuReq *req) {
    if (!gate) return kIOReturnNotPermitted;
    return gate->runAction(gatedSetCpuCppc, const_cast<struct UM790SetCpuReq *>(req));
}

IOReturn UM790Fan::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    if (stopped) return kIOReturnNotReady;
    UM790FanUserClient *uc = OSTypeAlloc(UM790FanUserClient);
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask(owningTask, securityID, type) ||
        !uc->attach(this) ||
        !uc->start(this)) {
        uc->release();
        return kIOReturnError;
    }
    *handler = uc;
    return kIOReturnSuccess;
}

bool UM790FanUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type)) return false;
    owner = nullptr;
    return true;
}

bool UM790FanUserClient::start(IOService *provider) {
    if (!IOUserClient::start(provider)) return false;
    owner = OSDynamicCast(UM790Fan, provider);
    return owner != nullptr;
}

void UM790FanUserClient::stop(IOService *provider) {
    owner = nullptr;
    IOUserClient::stop(provider);
}

IOReturn UM790FanUserClient::clientClose() {
    owner = nullptr;
    terminate();
    return kIOReturnSuccess;
}

IOReturn UM790FanUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                            IOExternalMethodDispatch *, OSObject *, void *) {
    if (!owner || isInactive()) return kIOReturnNotReady;

    switch (selector) {
    case kUM790MethodGetStatus: {
        if (!a || !a->structureOutput || a->structureOutputSize < sizeof(struct UM790FanStatus))
            return kIOReturnBadArgument;
        IOReturn rc = owner->handleGetStatus(static_cast<struct UM790FanStatus *>(a->structureOutput));
        if (rc == kIOReturnSuccess)
            a->structureOutputSize = sizeof(struct UM790FanStatus);
        return rc;
    }
    case kUM790MethodSetMode:
        if (!a || !a->structureInput || a->structureInputSize != sizeof(struct UM790SetModeReq))
            return kIOReturnBadArgument;
        return owner->handleSetMode(static_cast<const struct UM790SetModeReq *>(a->structureInput));
    case kUM790MethodSetCurve:
        if (!a || !a->structureInput || a->structureInputSize != sizeof(struct UM790SetCurveReq))
            return kIOReturnBadArgument;
        return owner->handleSetCurve(static_cast<const struct UM790SetCurveReq *>(a->structureInput));
    case kUM790MethodPingManual:
        if (!a || !a->structureInput || a->structureInputSize != sizeof(struct UM790PingReq))
            return kIOReturnBadArgument;
        return owner->handlePingManual(static_cast<const struct UM790PingReq *>(a->structureInput));
    case kUM790MethodGetCpuStatus: {
        if (!a || !a->structureOutput || a->structureOutputSize < sizeof(struct UM790CpuStatus))
            return kIOReturnBadArgument;
        IOReturn rc = owner->handleGetCpuStatus(static_cast<struct UM790CpuStatus *>(a->structureOutput));
        if (rc == kIOReturnSuccess)
            a->structureOutputSize = sizeof(struct UM790CpuStatus);
        return rc;
    }
    case kUM790MethodSetCpuCppc:
        if (!a || !a->structureInput || a->structureInputSize != sizeof(struct UM790SetCpuReq))
            return kIOReturnBadArgument;
        return owner->handleSetCpuCppc(static_cast<const struct UM790SetCpuReq *>(a->structureInput));
    default:
        return kIOReturnUnsupported;
    }
}
