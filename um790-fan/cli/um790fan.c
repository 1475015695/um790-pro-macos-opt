#include "../UM790FanABI.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sig(int sig) {
    (void)sig;
    g_stop = 1;
}

static io_connect_t open_fan_service(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching(UM790_FAN_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, "Error: %s service not found (is UM790Fan.kext loaded?)\n",
                UM790_FAN_SERVICE_NAME);
        return IO_OBJECT_NULL;
    }
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        fprintf(stderr, "Error: IOServiceOpen(%s) failed: 0x%x (%s)\n",
                UM790_FAN_SERVICE_NAME, kr, mach_error_string(kr));
        return IO_OBJECT_NULL;
    }
    return conn;
}

static int get_status(io_connect_t conn, struct UM790FanStatus *st) {
    size_t outSize = sizeof(*st);
    memset(st, 0, sizeof(*st));
    kern_return_t kr = IOConnectCallStructMethod(conn, kUM790MethodGetStatus,
                                                 NULL, 0, st, &outSize);
    if (kr != KERN_SUCCESS || outSize != sizeof(*st)) {
        fprintf(stderr, "Error: kUM790MethodGetStatus failed: 0x%x (size=%zu)\n", kr, outSize);
        return -1;
    }
    if (st->abi_version != UM790_FAN_ABI_VERSION) {
        fprintf(stderr, "Error: ABI mismatch (got %u, expected %u)\n",
                st->abi_version, UM790_FAN_ABI_VERSION);
        return -1;
    }
    return 0;
}

static const char *mode_str(uint8_t m) {
    switch (m) {
    case UM790_MODE_MANUAL:   return "manual";
    case UM790_MODE_CURVE:    return "curve";
    case UM790_MODE_FIRMWARE: return "firmware";
    default:                  return "unknown";
    }
}

static const char *preset_str(uint32_t p) {
    switch (p) {
    case UM790_PRESET_SILENT:   return "silent";
    case UM790_PRESET_BALANCED: return "balanced";
    case UM790_PRESET_COOL:     return "cool";
    case UM790_PRESET_FIRMWARE: return "firmware";
    case UM790_PRESET_CUSTOM:   return "custom";
    default:                    return "unknown";
    }
}

static void print_curve(const struct UM790Curve *c) {
    for (uint8_t i = 0; i < c->n && i < UM790_CURVE_MAX_POINTS; i++) {
        if (i) printf(",");
        if (c->code[i] == UM790_CURVE_FIRMWARE) {
            printf("%u:fw", c->temp[i]);
        } else {
            printf("%u:%d", c->temp[i], (int)c->code[i] * 100);
        }
    }
}

static void print_status_text(const struct UM790FanStatus *st) {
    printf("UM790Fan (admitted=%u, profile=%s, preset=%s, read_failures=%u)\n",
           st->admitted,
           st->profile_name[0] ? st->profile_name : "?",
           preset_str(st->preset_id),
           st->read_failures);
    if (!st->admitted && st->admit_error[0]) {
        printf("  admit_error: %s\n", st->admit_error);
    }
    const char *names[UM790_NR_FANS] = { "CPU", "System" };
    for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
        printf("  %-6s: %3u C | %4d RPM | mode=%-8s | owned=%u | cur=%4d RPM | duty=%3u | hot=%u hold=%u hunts=%u | curve=",
               names[i],
               st->temp_c[i],
               st->rpm[i],
               mode_str(st->mode[i]),
               st->owned[i],
               st->cur_code[i] >= 0 ? (int)st->cur_code[i] * 100 : -1,
               st->duty[i],
               st->hot[i],
               st->holding[i],
               st->hunts[i]);
        print_curve(&st->curves[i]);
        printf("\n");
    }
    printf("  sys_ec: effective=0x%02x override=0x%02x target=%u (%d RPM)\n",
           st->sys_effective, st->sys_override, st->sys_target, (int)st->sys_target * 100);
}

static void print_status_json(const struct UM790FanStatus *st) {
    printf("{");
    printf("\"abi_version\":%u,\"admitted\":%s,\"profile\":\"%s\",\"preset\":\"%s\",\"read_failures\":%u,",
           st->abi_version,
           st->admitted ? "true" : "false",
           st->profile_name[0] ? st->profile_name : "",
           preset_str(st->preset_id),
           st->read_failures);
    printf("\"admit_error\":\"%s\",", st->admit_error);
    printf("\"pnp\":[%u,%u,%u],", st->pnp[0], st->pnp[1], st->pnp[2]);
    printf("\"sys_ec\":{\"effective\":%u,\"override\":%u,\"target\":%u},",
           st->sys_effective, st->sys_override, st->sys_target);
    printf("\"fans\":[");
    const char *names[UM790_NR_FANS] = { "cpu", "system" };
    for (uint32_t i = 0; i < UM790_NR_FANS; i++) {
        if (i) printf(",");
        printf("{\"name\":\"%s\",\"temp_c\":%u,\"rpm\":%d,\"mode\":\"%s\",\"owned\":%s,"
               "\"cur_rpm\":%d,\"manual_rpm\":%d,\"duty\":%u,\"hot\":%s,\"holding\":%s,\"hunts\":%u}",
               names[i], st->temp_c[i], st->rpm[i], mode_str(st->mode[i]),
               st->owned[i] ? "true" : "false",
               st->cur_code[i] >= 0 ? (int)st->cur_code[i] * 100 : -1,
               st->manual_code[i] >= 0 ? (int)st->manual_code[i] * 100 : -1,
               st->duty[i],
               st->hot[i] ? "true" : "false",
               st->holding[i] ? "true" : "false",
               st->hunts[i]);
    }
    printf("]}\n");
}

static void print_dump(const struct UM790FanStatus *st) {
    print_status_text(st);
    printf("  pnp   : %02x %02x %02x\n", st->pnp[0], st->pnp[1], st->pnp[2]);
    printf("  ident : %02x %02x %02x %02x %02x %02x\n",
           st->ident[0], st->ident[1], st->ident[2],
           st->ident[3], st->ident[4], st->ident[5]);
    printf("  cpu_table: selector=0x%02x (%s), temp_override=0x%02x\n",
           st->cpu_selector,
           st->profile_name[0] ? st->profile_name : "?",
           st->cpu_temp_override);
    printf("    row  base  upper  lower  slope\n");
    for (int r = 0; r < 8; r++) {
        printf("    %3d  %4u  %5u  %5u  %5u%s\n",
               r,
               st->cpu_rows[r][0], st->cpu_rows[r][1], st->cpu_rows[r][2],
               st->cpu_slopes[r],
               r == 7 ? "  (critical)" : "");
    }
    printf("  sys_policy:\n");
    for (int r = 0; r < 3; r++) {
        printf("    row %d: code=%u (%u RPM) upper=%u lower=%u\n",
               r, st->sys_policy_rows[r][0], (unsigned)st->sys_policy_rows[r][0] * 100,
               st->sys_policy_rows[r][1], st->sys_policy_rows[r][2]);
    }
    printf("    tail : %02x %02x %02x\n",
           st->sys_policy_tail[0], st->sys_policy_tail[1], st->sys_policy_tail[2]);
}

static int parse_fan_id(const char *s, bool allow_all, uint32_t *out) {
    if (!strcmp(s, "cpu")) {
        *out = UM790_FAN_CPU;
        return 0;
    }
    if (!strcmp(s, "sys") || !strcmp(s, "system")) {
        *out = UM790_FAN_SYS;
        return 0;
    }
    if (allow_all && !strcmp(s, "all")) {
        *out = UM790_FAN_ALL;
        return 0;
    }
    return -1;
}

static int call_set_mode(io_connect_t conn, uint32_t fan_id, uint32_t mode, int32_t rpm, uint32_t preset_id) {
    struct UM790SetModeReq req;
    memset(&req, 0, sizeof(req));
    req.fan_id = fan_id;
    req.mode = mode;
    req.rpm = rpm;
    req.preset_id = preset_id;
    kern_return_t kr = IOConnectCallStructMethod(conn, kUM790MethodSetMode,
                                                 &req, sizeof(req), NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Error: SetMode failed: 0x%x (%s)\n", kr, mach_error_string(kr));
        return -1;
    }
    return 0;
}

static int call_set_curve(io_connect_t conn, uint32_t fan_id, uint32_t preset_id, const struct UM790Curve *c) {
    struct UM790SetCurveReq req;
    memset(&req, 0, sizeof(req));
    req.fan_id = fan_id;
    req.preset_id = preset_id;
    req.curve = *c;
    kern_return_t kr = IOConnectCallStructMethod(conn, kUM790MethodSetCurve,
                                                 &req, sizeof(req), NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Error: SetCurve failed: 0x%x (%s)\n", kr, mach_error_string(kr));
        return -1;
    }
    return 0;
}

static int parse_curve_arg(const char *arg, uint32_t fan_id, struct UM790Curve *out) {
    memset(out, 0, sizeof(*out));
    int min_code = (fan_id == UM790_FAN_CPU) ? UM790_CPU_MIN_CODE : UM790_SYS_MIN_CODE;
    char buf[512];
    if (strlen(arg) >= sizeof(buf)) return -1;
    strcpy(buf, arg);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        if (out->n >= UM790_CURVE_MAX_POINTS) return -1;
        char *colon = strchr(tok, ':');
        if (!colon) return -1;
        *colon = '\0';
        char *tstr = tok;
        char *vstr = colon + 1;

        char *endp = NULL;
        errno = 0;
        long temp = strtol(tstr, &endp, 10);
        if (errno || !endp || *endp != '\0' || temp < 0 || temp > 120) return -1;
        if (out->n > 0 && temp <= out->temp[out->n - 1]) return -1;

        int8_t code = 0;
        if (!strcmp(vstr, "fw") || !strcmp(vstr, "firmware")) {
            code = UM790_CURVE_FIRMWARE;
        } else {
            long rpm = strtol(vstr, &endp, 10);
            if (errno || !endp || *endp != '\0' || rpm < 0 || rpm > UM790_CODE_MAX * 100) return -1;
            long c = (rpm + 50) / 100;
            if (rpm > 0 && c == 0) return -1;
            if (c > 0 && c < min_code) return -1;
            code = (int8_t)c;
        }
        out->temp[out->n] = (uint8_t)temp;
        out->code[out->n] = code;
        out->n++;
    }
    return out->n > 0 ? 0 : -1;
}

static void usage(void) {
    fprintf(stderr,
            "Usage:\n"
            "  um790fan [status] [--json]\n"
            "  um790fan dump\n"
            "  um790fan preset <silent|balanced|cool|firmware>\n"
            "  um790fan mode <cpu|sys|all> <curve|firmware>\n"
            "  um790fan manual <cpu|sys|all> <rpm> [--hold]\n"
            "  um790fan curve <cpu|sys> <temp:rpm,...>   (e.g. 0:0,25:1000,45:1400,66:firmware)\n"
            "  um790fan cpu [perf|balanced|powersave|epp <0..255>]\n");
}

static const char *cpu_profile_str(uint32_t pid) {
    switch (pid) {
    case UM790_CPU_PROFILE_PERF:      return "perf";
    case UM790_CPU_PROFILE_BALANCED:  return "balanced";
    case UM790_CPU_PROFILE_POWERSAVE: return "powersave";
    case UM790_CPU_PROFILE_CUSTOM:    return "custom";
    default:                          return "unknown";
    }
}

static int get_cpu_status(io_connect_t conn, struct UM790CpuStatus *st) {
    size_t sz = sizeof(*st);
    memset(st, 0, sizeof(*st));
    kern_return_t kr = IOConnectCallStructMethod(conn, kUM790MethodGetCpuStatus,
                                                 NULL, 0, st, &sz);
    if (kr != KERN_SUCCESS || sz != sizeof(*st)) {
        fprintf(stderr, "Error: GetCpuStatus failed: 0x%x (%s)\n", kr, mach_error_string(kr));
        return -1;
    }
    return 0;
}

static int set_cpu_cppc(io_connect_t conn, uint32_t pid, uint8_t epp, uint8_t use_low_nonlin) {
    struct UM790SetCpuReq req = {
        .profile_id = pid,
        .epp = epp,
        .use_low_nonlin = use_low_nonlin,
    };
    kern_return_t kr = IOConnectCallStructMethod(conn, kUM790MethodSetCpuCppc,
                                                 &req, sizeof(req), NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Error: SetCpuCppc failed: 0x%x (%s)\n", kr, mach_error_string(kr));
        return -1;
    }
    return 0;
}

static void print_cpu_status(const struct UM790CpuStatus *cs) {
    int cpb_dis = (int)((cs->hwcr_raw >> 25) & 1ULL);
    int cur_pstate = (int)(cs->pstate_stat_raw & 0x7ULL);
    printf("UM790 CPU CPPC (supported=%u, enabled=%u, profile=%s, status=%s, errors=%u)\n",
           cs->supported, cs->enabled, cpu_profile_str(cs->profile_id),
           cs->status_msg[0] ? cs->status_msg : "?", cs->apply_errors);
    printf("  CAP1 : lowest=%u low_nonlin=%u nominal=%u highest=%u (raw=0x%016llx)\n",
           cs->cap_lowest, cs->cap_low_nonlin, cs->cap_nominal, cs->cap_highest,
           (unsigned long long)cs->cap1_raw);
    printf("  REQ  : min=%u max=%u epp=0x%02x (%u) low_nonlin=%u (raw=0x%016llx, en=0x%llx)\n",
           cs->min_perf, cs->max_perf, cs->epp, cs->epp, cs->use_low_nonlin,
           (unsigned long long)cs->req_raw, (unsigned long long)cs->en_raw);
    printf("  HWCR : CPB=%s (raw=0x%016llx) | PSTATE_STAT=%d (raw=0x%016llx)\n",
           cpb_dis ? "DISABLED" : "enabled", (unsigned long long)cs->hwcr_raw,
           cur_pstate, (unsigned long long)cs->pstate_stat_raw);
}

int main(int argc, char **argv) {
    const char *cmd = (argc >= 2) ? argv[1] : "status";
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage();
        return 0;
    }

    io_connect_t conn = open_fan_service();
    if (conn == IO_OBJECT_NULL) return 1;

    int rc = 0;
    if (!strcmp(cmd, "status")) {
        bool json = (argc >= 3 && !strcmp(argv[2], "--json"));
        struct UM790FanStatus st;
        if (get_status(conn, &st) != 0) rc = 1;
        else if (json) print_status_json(&st);
        else print_status_text(&st);
    } else if (!strcmp(cmd, "dump")) {
        struct UM790FanStatus st;
        if (get_status(conn, &st) != 0) rc = 1;
        else print_dump(&st);
    } else if (!strcmp(cmd, "preset")) {
        if (argc < 3) { usage(); rc = 2; goto out; }
        const char *p = argv[2];
        if (!strcmp(p, "firmware") || !strcmp(p, "fw")) {
            if (call_set_mode(conn, UM790_FAN_ALL, UM790_MODE_FIRMWARE, 0, UM790_PRESET_FIRMWARE) != 0) rc = 1;
        } else {
            uint32_t pid = 0;
            if (!strcmp(p, "silent")) pid = UM790_PRESET_SILENT;
            else if (!strcmp(p, "balanced")) pid = UM790_PRESET_BALANCED;
            else if (!strcmp(p, "cool")) pid = UM790_PRESET_COOL;
            else { usage(); rc = 2; goto out; }

            struct UM790Curve cpu, sys;
            um790_fill_preset_curves(pid, &cpu, &sys);
            if (call_set_curve(conn, UM790_FAN_CPU, pid, &cpu) != 0 ||
                call_set_curve(conn, UM790_FAN_SYS, pid, &sys) != 0 ||
                call_set_mode(conn, UM790_FAN_ALL, UM790_MODE_CURVE, 0, pid) != 0) {
                rc = 1;
            }
        }
    } else if (!strcmp(cmd, "mode")) {
        if (argc < 4) { usage(); rc = 2; goto out; }
        uint32_t fan_id = 0;
        if (parse_fan_id(argv[2], true, &fan_id) != 0) { usage(); rc = 2; goto out; }
        uint32_t mode = 0;
        uint32_t pid = UM790_PRESET_CUSTOM;
        if (!strcmp(argv[3], "curve")) mode = UM790_MODE_CURVE;
        else if (!strcmp(argv[3], "firmware") || !strcmp(argv[3], "fw")) {
            mode = UM790_MODE_FIRMWARE;
            if (fan_id == UM790_FAN_ALL) pid = UM790_PRESET_FIRMWARE;
        } else { usage(); rc = 2; goto out; }
        if (call_set_mode(conn, fan_id, mode, 0, pid) != 0) rc = 1;
    } else if (!strcmp(cmd, "manual")) {
        if (argc < 4) { usage(); rc = 2; goto out; }
        uint32_t fan_id = 0;
        if (parse_fan_id(argv[2], true, &fan_id) != 0) { usage(); rc = 2; goto out; }
        char *endp = NULL;
        errno = 0;
        long rpm = strtol(argv[3], &endp, 10);
        if (errno || !endp || *endp != '\0' || rpm < 0 || rpm > UM790_CODE_MAX * 100) {
            fprintf(stderr, "Error: invalid RPM '%s'\n", argv[3]);
            rc = 2;
            goto out;
        }
        bool hold = (argc >= 5 && !strcmp(argv[4], "--hold"));
        if (call_set_mode(conn, fan_id, UM790_MODE_MANUAL, (int32_t)rpm, UM790_PRESET_CUSTOM) != 0) {
            rc = 1;
            goto out;
        }
        if (hold) {
            signal(SIGINT, on_sig);
            signal(SIGTERM, on_sig);
            printf("Holding manual %ld RPM on %s (Ctrl-C to release back to firmware)...\n", rpm, argv[2]);
            fflush(stdout);
            while (!g_stop) {
                sleep(15);
                if (g_stop) break;
                struct UM790PingReq preq = { .fan_id = fan_id };
                IOConnectCallStructMethod(conn, kUM790MethodPingManual,
                                          &preq, sizeof(preq), NULL, NULL);
            }
            call_set_mode(conn, fan_id, UM790_MODE_FIRMWARE, 0, UM790_PRESET_FIRMWARE);
            printf("Released back to firmware.\n");
        }
    } else if (!strcmp(cmd, "curve")) {
        if (argc < 4) { usage(); rc = 2; goto out; }
        uint32_t fan_id = 0;
        if (parse_fan_id(argv[2], false, &fan_id) != 0) { usage(); rc = 2; goto out; }
        struct UM790Curve c;
        if (parse_curve_arg(argv[3], fan_id, &c) != 0) {
            fprintf(stderr, "Error: invalid curve '%s'\n", argv[3]);
            rc = 2;
            goto out;
        }
        if (call_set_curve(conn, fan_id, UM790_PRESET_CUSTOM, &c) != 0 ||
            call_set_mode(conn, fan_id, UM790_MODE_CURVE, 0, UM790_PRESET_CUSTOM) != 0) {
            rc = 1;
        }
    } else if (!strcmp(cmd, "cpu")) {
        if (argc == 2) {
            struct UM790CpuStatus cs;
            if (get_cpu_status(conn, &cs) != 0) rc = 1;
            else print_cpu_status(&cs);
        } else if (!strcmp(argv[2], "perf") || !strcmp(argv[2], "performance")) {
            rc = set_cpu_cppc(conn, UM790_CPU_PROFILE_PERF, 0x00, 1) == 0 ? 0 : 1;
        } else if (!strcmp(argv[2], "balanced")) {
            rc = set_cpu_cppc(conn, UM790_CPU_PROFILE_BALANCED, 0x20, 0) == 0 ? 0 : 1;
        } else if (!strcmp(argv[2], "powersave") || !strcmp(argv[2], "eco")) {
            rc = set_cpu_cppc(conn, UM790_CPU_PROFILE_POWERSAVE, 0xA0, 0) == 0 ? 0 : 1;
        } else if (!strcmp(argv[2], "epp") && argc >= 4) {
            char *endp = NULL;
            errno = 0;
            long v = strtol(argv[3], &endp, 0);
            if (errno || !endp || *endp != '\0' || v < 0 || v > 255) {
                fprintf(stderr, "Error: invalid EPP '%s' (expected 0..255)\n", argv[3]);
                rc = 2;
            } else {
                rc = set_cpu_cppc(conn, UM790_CPU_PROFILE_CUSTOM, (uint8_t)v, 0) == 0 ? 0 : 1;
            }
        } else {
            usage();
            rc = 2;
        }
    } else {
        usage();
        rc = 2;
    }

out:
    IOServiceClose(conn);
    return rc;
}
