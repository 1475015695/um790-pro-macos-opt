#ifndef UM790_FAN_ABI_H
#define UM790_FAN_ABI_H

#include <stdint.h>
#include <string.h>

#define UM790_FAN_SERVICE_NAME "UM790Fan"
#define UM790_FAN_ABI_VERSION  1

enum {
    UM790_FAN_CPU = 0,
    UM790_FAN_SYS = 1,
    UM790_NR_FANS = 2,
    UM790_FAN_ALL = 0xFFFFFFFFu,
};

enum {
    UM790_MODE_MANUAL   = 1,
    UM790_MODE_CURVE    = 2,
    UM790_MODE_FIRMWARE = 3,
};

enum {
    UM790_PRESET_CUSTOM   = 0,
    UM790_PRESET_SILENT   = 1,
    UM790_PRESET_BALANCED = 2,
    UM790_PRESET_COOL     = 3,
    UM790_PRESET_FIRMWARE = 4,
};

enum {
    UM790_CURVE_MAX_POINTS = 8,
    UM790_CURVE_FIRMWARE   = -1,
    UM790_CODE_MAX         = 51,
    UM790_CPU_MIN_CODE     = 10,
    UM790_SYS_MIN_CODE     = 8,
    UM790_CPU_TEMP_LIMIT   = 80,
    UM790_SYS_TEMP_LIMIT   = 55,
};

enum {
    kUM790MethodGetStatus    = 0,
    kUM790MethodSetMode      = 1,
    kUM790MethodSetCurve     = 2,
    kUM790MethodPingManual   = 3,
    kUM790MethodGetCpuStatus = 4,
    kUM790MethodSetCpuCppc   = 5,
};

enum {
    UM790_CPU_PROFILE_CUSTOM    = 0,
    UM790_CPU_PROFILE_PERF      = 1,
    UM790_CPU_PROFILE_BALANCED  = 2,
    UM790_CPU_PROFILE_POWERSAVE = 3,
};

struct UM790Curve {
    uint8_t n;
    uint8_t temp[UM790_CURVE_MAX_POINTS];
    int8_t  code[UM790_CURVE_MAX_POINTS];
    uint8_t _pad[7];
};

struct UM790FanStatus {
    uint32_t abi_version;
    uint32_t admitted;

    uint8_t  pnp[3];
    uint8_t  ident[6];
    uint8_t  cpu_selector;
    uint8_t  cpu_temp_override;
    uint8_t  _pad0;

    uint8_t  cpu_rows[8][3];
    uint8_t  cpu_slopes[8];
    uint8_t  sys_policy_rows[3][3];
    uint8_t  sys_policy_tail[3];

    uint8_t  temp_c[UM790_NR_FANS];
    uint8_t  duty[UM790_NR_FANS];
    int32_t  rpm[UM790_NR_FANS];

    uint8_t  sys_effective;
    uint8_t  sys_override;
    uint8_t  sys_target;
    uint8_t  _pad1;

    uint8_t  mode[UM790_NR_FANS];
    uint8_t  owned[UM790_NR_FANS];
    uint8_t  hot[UM790_NR_FANS];
    uint8_t  holding[UM790_NR_FANS];

    int16_t  cur_code[UM790_NR_FANS];
    int16_t  manual_code[UM790_NR_FANS];
    uint8_t  curve_level[UM790_NR_FANS];
    uint8_t  hunts[UM790_NR_FANS];

    uint32_t preset_id;
    uint32_t read_failures;

    char     profile_name[16];
    char     admit_error[96];

    struct UM790Curve curves[UM790_NR_FANS];
};

struct UM790SetModeReq {
    uint32_t fan_id;
    uint32_t mode;
    int32_t  rpm;
    uint32_t preset_id;
};

struct UM790SetCurveReq {
    uint32_t fan_id;
    uint32_t preset_id;
    struct UM790Curve curve;
};

struct UM790PingReq {
    uint32_t fan_id;
};

struct UM790CpuStatus {
    uint32_t abi_version;
    uint32_t supported;
    uint32_t enabled;
    uint32_t profile_id;
    uint8_t  epp;
    uint8_t  min_perf;
    uint8_t  max_perf;
    uint8_t  use_low_nonlin;
    uint8_t  cap_lowest;
    uint8_t  cap_low_nonlin;
    uint8_t  cap_nominal;
    uint8_t  cap_highest;
    uint32_t apply_errors;
    uint64_t cap1_raw;
    uint64_t en_raw;
    uint64_t req_raw;
    uint64_t hwcr_raw;
    uint64_t pstate_stat_raw;
    char     status_msg[96];
};

struct UM790SetCpuReq {
    uint32_t profile_id;
    uint8_t  epp;
    uint8_t  use_low_nonlin;
    uint8_t  _pad[2];
};

static inline void um790_fill_preset_curves(uint32_t preset_id,
                                            struct UM790Curve *cpu,
                                            struct UM790Curve *sys) {
    if (cpu) {
        memset(cpu, 0, sizeof(*cpu));
    }
    if (sys) {
        memset(sys, 0, sizeof(*sys));
    }

    switch (preset_id) {
    case UM790_PRESET_BALANCED:
        if (cpu) {
            cpu->n = 6;
            cpu->temp[0] = 0;  cpu->code[0] = 0;
            cpu->temp[1] = 25; cpu->code[1] = 12;
            cpu->temp[2] = 45; cpu->code[2] = 16;
            cpu->temp[3] = 54; cpu->code[3] = 20;
            cpu->temp[4] = 64; cpu->code[4] = 25;
            cpu->temp[5] = 72; cpu->code[5] = UM790_CURVE_FIRMWARE;
        }
        if (sys) {
            sys->n = 4;
            sys->temp[0] = 0;  sys->code[0] = 0;
            sys->temp[1] = 25; sys->code[1] = 12;
            sys->temp[2] = 40; sys->code[2] = 18;
            sys->temp[3] = 50; sys->code[3] = UM790_CURVE_FIRMWARE;
        }
        break;
    case UM790_PRESET_COOL:
        if (cpu) {
            cpu->n = 5;
            cpu->temp[0] = 0;  cpu->code[0] = 16;
            cpu->temp[1] = 40; cpu->code[1] = 22;
            cpu->temp[2] = 52; cpu->code[2] = 28;
            cpu->temp[3] = 64; cpu->code[3] = 34;
            cpu->temp[4] = 74; cpu->code[4] = UM790_CURVE_FIRMWARE;
        }
        if (sys) {
            sys->n = 4;
            sys->temp[0] = 0;  sys->code[0] = 16;
            sys->temp[1] = 38; sys->code[1] = 22;
            sys->temp[2] = 48; sys->code[2] = 28;
            sys->temp[3] = 54; sys->code[3] = UM790_CURVE_FIRMWARE;
        }
        break;
    case UM790_PRESET_SILENT:
    default:
        if (cpu) {
            cpu->n = 5;
            cpu->temp[0] = 0;  cpu->code[0] = 0;
            cpu->temp[1] = 25; cpu->code[1] = 10;
            cpu->temp[2] = 45; cpu->code[2] = 14;
            cpu->temp[3] = 54; cpu->code[3] = 18;
            cpu->temp[4] = 66; cpu->code[4] = UM790_CURVE_FIRMWARE;
        }
        if (sys) {
            sys->n = 4;
            sys->temp[0] = 0;  sys->code[0] = 0;
            sys->temp[1] = 25; sys->code[1] = 10;
            sys->temp[2] = 40; sys->code[2] = 16;
            sys->temp[3] = 50; sys->code[3] = UM790_CURVE_FIRMWARE;
        }
        break;
    }
}

#endif /* UM790_FAN_ABI_H */
