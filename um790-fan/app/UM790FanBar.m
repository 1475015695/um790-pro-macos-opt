#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#include <signal.h>
#include "UM790FanABI.h"

static void logLine(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void logLine(NSString *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString *body = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSLog(@"[UM790FanBar] %@", body);
    fprintf(stdout, "[UM790FanBar] %s\n", body.UTF8String);
    fflush(stdout);
}

@interface UM790FanBarApp : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong, nonatomic) NSStatusItem *statusItem;
@property (strong, nonatomic) NSMenu *menu;
@property (strong, nonatomic) NSMenuItem *cpuInfoItem;
@property (strong, nonatomic) NSMenuItem *sysInfoItem;
@property (strong, nonatomic) NSMenuItem *ecInfoItem;
@property (strong, nonatomic) NSMenuItem *presetSilentItem;
@property (strong, nonatomic) NSMenuItem *presetBalancedItem;
@property (strong, nonatomic) NSMenuItem *presetCoolItem;
@property (strong, nonatomic) NSMenuItem *presetFirmwareItem;
@property (strong, nonatomic) NSMenuItem *cpuManualParent;
@property (strong, nonatomic) NSMenuItem *sysManualParent;
@property (strong, nonatomic) NSMenu *cpuManualMenu;
@property (strong, nonatomic) NSMenu *sysManualMenu;
@property (strong, nonatomic) NSMenuItem *cpuCppcParent;
@property (strong, nonatomic) NSMenu *cpuCppcMenu;
@property (strong, nonatomic) NSMenuItem *cppcPerfItem;
@property (strong, nonatomic) NSMenuItem *cppcBalItem;
@property (strong, nonatomic) NSMenuItem *cppcEcoItem;
@property (strong, nonatomic) NSMenuItem *watchToggleItem;
@property (strong, nonatomic) NSMenuItem *watchStatusItem;
@property (strong, nonatomic) NSMenuItem *watchParent;
@property (strong, nonatomic) NSMenu *watchMenu;
@property (strong, nonatomic) NSMenuItem *watchNotifyItem;
@property (strong, nonatomic) NSMenuItem *watchEventsAnchorSep;
@property (strong, nonatomic) NSMutableArray<NSMenuItem *> *watchEventItems;

@property (nonatomic) BOOL watchEnabled;
@property (nonatomic) BOOL watchRunning;
@property (nonatomic) BOOL watchNotify;
@property (nonatomic) BOOL watchBusy;
@property (nonatomic) NSInteger watchUnreadCritical;
@property (nonatomic) NSInteger watchUnreadWarn;
@property (copy, nonatomic) NSDictionary *watchMetrics;
@property (copy, nonatomic) NSArray<NSDictionary *> *watchRecentEvents;

@property (nonatomic) io_connect_t conn;
@property (nonatomic) BOOL hasStatus;
@property (nonatomic) struct UM790FanStatus status;
@property (nonatomic) BOOL hasCpuStatus;
@property (nonatomic) BOOL cpuMethodUnsupported;
@property (nonatomic) struct UM790CpuStatus cpuStatus;
@property (nonatomic) BOOL appliedInitialConfig;
@property (nonatomic) uint32_t desiredPreset;
@property (nonatomic) uint32_t desiredCpuProfile;
@property (nonatomic) NSTimeInterval lastReconnectAttempt;
@property (nonatomic) NSTimeInterval lastManualPing;
@property (strong, nonatomic) NSTimer *pollTimer;
@end

static NSString *configFilePath(void) {
    NSArray<NSString *> *dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *base = dirs.firstObject ?: [@"~/Library/Application Support" stringByExpandingTildeInPath];
    return [[base stringByAppendingPathComponent:@"UM790Fan"] stringByAppendingPathComponent:@"config.json"];
}

static NSString *presetKeyFromID(uint32_t pid) {
    switch (pid) {
    case UM790_PRESET_BALANCED: return @"balanced";
    case UM790_PRESET_COOL:     return @"cool";
    case UM790_PRESET_FIRMWARE: return @"firmware";
    case UM790_PRESET_SILENT:
    default:                    return @"silent";
    }
}

static uint32_t presetIDFromKey(NSString *key) {
    if ([key isEqualToString:@"balanced"]) return UM790_PRESET_BALANCED;
    if ([key isEqualToString:@"cool"])     return UM790_PRESET_COOL;
    if ([key isEqualToString:@"firmware"]) return UM790_PRESET_FIRMWARE;
    return UM790_PRESET_SILENT;
}

static NSString *cpuProfileKeyFromID(uint32_t pid) {
    switch (pid) {
    case UM790_CPU_PROFILE_PERF:      return @"perf";
    case UM790_CPU_PROFILE_POWERSAVE: return @"powersave";
    case UM790_CPU_PROFILE_BALANCED:
    default:                          return @"balanced";
    }
}

static uint32_t cpuProfileIDFromKey(NSString *key) {
    if ([key isEqualToString:@"perf"])      return UM790_CPU_PROFILE_PERF;
    if ([key isEqualToString:@"powersave"]) return UM790_CPU_PROFILE_POWERSAVE;
    return UM790_CPU_PROFILE_BALANCED;
}

static NSString *modeLabel(uint8_t mode, int16_t curCode, uint8_t owned, uint8_t hot, uint8_t holding) {
    if (hot)     return @"高温交还原厂";
    if (holding) return @"防震荡退避中";
    switch (mode) {
    case UM790_MODE_MANUAL:
        return [NSString stringWithFormat:@"手动 %d RPM", curCode >= 0 ? (int)curCode * 100 : 0];
    case UM790_MODE_CURVE:
        if (!owned) return @"曲线(原厂区间)";
        return [NSString stringWithFormat:@"曲线 %d RPM", curCode >= 0 ? (int)curCode * 100 : 0];
    case UM790_MODE_FIRMWARE:
    default:
        return @"原厂自动";
    }
}

@implementation UM790FanBarApp

- (void)dealloc {
    [self closeConn];
}

- (void)closeConn {
    if (_conn != IO_OBJECT_NULL) {
        IOServiceClose(_conn);
        _conn = IO_OBJECT_NULL;
    }
}

- (BOOL)ensureConnection {
    if (_conn != IO_OBJECT_NULL) return YES;
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (_lastReconnectAttempt > 0 && (now - _lastReconnectAttempt) < 2.8) {
        return NO;
    }
    _lastReconnectAttempt = now;

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching(UM790_FAN_SERVICE_NAME));
    if (!svc) return NO;

    io_connect_t c = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS || c == IO_OBJECT_NULL) {
        return NO;
    }
    _conn = c;
    _appliedInitialConfig = NO;
    logLine(@"Connected to %s service", UM790_FAN_SERVICE_NAME);
    return YES;
}

- (BOOL)fetchStatus {
    if (![self ensureConnection]) {
        _hasStatus = NO;
        return NO;
    }
    struct UM790FanStatus st;
    memset(&st, 0, sizeof(st));
    size_t sz = sizeof(st);
    kern_return_t kr = IOConnectCallStructMethod(_conn, kUM790MethodGetStatus, NULL, 0, &st, &sz);
    if (kr != KERN_SUCCESS || sz != sizeof(st) || st.abi_version != UM790_FAN_ABI_VERSION) {
        logLine(@"GetStatus failed (kr=0x%x, sz=%zu), closing connection", kr, sz);
        [self closeConn];
        _hasStatus = NO;
        return NO;
    }
    _status = st;
    _hasStatus = YES;

    struct UM790CpuStatus cst;
    memset(&cst, 0, sizeof(cst));
    size_t csz = sizeof(cst);
    kern_return_t ckr = IOConnectCallStructMethod(_conn, kUM790MethodGetCpuStatus, NULL, 0, &cst, &csz);
    if (ckr == KERN_SUCCESS && csz == sizeof(cst) && cst.abi_version == UM790_FAN_ABI_VERSION) {
        _cpuStatus = cst;
        _hasCpuStatus = YES;
        _cpuMethodUnsupported = NO;
    } else {
        _hasCpuStatus = NO;
        _cpuMethodUnsupported = (ckr == kIOReturnUnsupported);
    }
    return YES;
}

- (void)loadSavedConfig {
    _desiredPreset = UM790_PRESET_SILENT;
    _desiredCpuProfile = UM790_CPU_PROFILE_BALANCED;
    NSString *path = configFilePath();
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) {
        [self saveConfig];
        return;
    }
    id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if ([obj isKindOfClass:[NSDictionary class]]) {
        NSString *p = obj[@"preset"];
        if ([p isKindOfClass:[NSString class]]) {
            _desiredPreset = presetIDFromKey(p);
        }
        NSString *cp = obj[@"cpu_profile"];
        if ([cp isKindOfClass:[NSString class]]) {
            _desiredCpuProfile = cpuProfileIDFromKey(cp);
        }
    }
}

- (void)saveConfig {
    NSString *path = configFilePath();
    NSString *dir = [path stringByDeletingLastPathComponent];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSDictionary *dict = @{
        @"preset": presetKeyFromID(_desiredPreset),
        @"cpu_profile": cpuProfileKeyFromID(_desiredCpuProfile),
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:dict options:NSJSONWritingPrettyPrinted error:nil];
    if (json) {
        [json writeToFile:path atomically:YES];
    }
}

- (void)saveConfigPreset:(uint32_t)presetID {
    _desiredPreset = presetID;
    [self saveConfig];
}

- (BOOL)applyCpuProfile:(uint32_t)pid persist:(BOOL)persist {
    if (_conn == IO_OBJECT_NULL || !_hasCpuStatus || !_cpuStatus.supported) return NO;
    struct UM790SetCpuReq req = { 0 };
    req.profile_id = pid;
    if (pid == UM790_CPU_PROFILE_PERF) { req.epp = 0x00; req.use_low_nonlin = 1; }
    else if (pid == UM790_CPU_PROFILE_POWERSAVE) { req.epp = 0xA0; req.use_low_nonlin = 0; }
    else { req.epp = 0x20; req.use_low_nonlin = 0; }
    kern_return_t kr = IOConnectCallStructMethod(_conn, kUM790MethodSetCpuCppc, &req, sizeof(req), NULL, NULL);
    if (kr != KERN_SUCCESS) {
        logLine(@"SetCpuCppc(%u) failed: 0x%x", pid, kr);
        return NO;
    }
    if (persist) {
        _desiredCpuProfile = pid;
        [self saveConfig];
    }
    return YES;
}

- (BOOL)applyPreset:(uint32_t)presetID persist:(BOOL)persist {
    if (_conn == IO_OBJECT_NULL) return NO;
    if (presetID == UM790_PRESET_FIRMWARE) {
        struct UM790SetModeReq req = {
            .fan_id = UM790_FAN_ALL,
            .mode = UM790_MODE_FIRMWARE,
            .rpm = 0,
            .preset_id = UM790_PRESET_FIRMWARE,
        };
        kern_return_t kr = IOConnectCallStructMethod(_conn, kUM790MethodSetMode,
                                                     &req, sizeof(req), NULL, NULL);
        if (kr != KERN_SUCCESS) {
            logLine(@"SetMode(firmware) failed: 0x%x", kr);
            return NO;
        }
    } else {
        struct UM790Curve cpu, sys;
        um790_fill_preset_curves(presetID, &cpu, &sys);
        struct UM790SetCurveReq rCpu = { .fan_id = UM790_FAN_CPU, .preset_id = presetID, .curve = cpu };
        struct UM790SetCurveReq rSys = { .fan_id = UM790_FAN_SYS, .preset_id = presetID, .curve = sys };
        struct UM790SetModeReq rMode = {
            .fan_id = UM790_FAN_ALL,
            .mode = UM790_MODE_CURVE,
            .rpm = 0,
            .preset_id = presetID,
        };
        if (IOConnectCallStructMethod(_conn, kUM790MethodSetCurve, &rCpu, sizeof(rCpu), NULL, NULL) != KERN_SUCCESS ||
            IOConnectCallStructMethod(_conn, kUM790MethodSetCurve, &rSys, sizeof(rSys), NULL, NULL) != KERN_SUCCESS ||
            IOConnectCallStructMethod(_conn, kUM790MethodSetMode, &rMode, sizeof(rMode), NULL, NULL) != KERN_SUCCESS) {
            logLine(@"Apply preset %u failed", presetID);
            return NO;
        }
    }
    if (persist) {
        [self saveConfigPreset:presetID];
    }
    return YES;
}

- (void)applyManualFan:(uint32_t)fanID rpm:(int32_t)rpm {
    if (_conn == IO_OBJECT_NULL) return;
    struct UM790SetModeReq req = {
        .fan_id = fanID,
        .mode = UM790_MODE_MANUAL,
        .rpm = rpm,
        .preset_id = UM790_PRESET_CUSTOM,
    };
    IOConnectCallStructMethod(_conn, kUM790MethodSetMode, &req, sizeof(req), NULL, NULL);
    _lastManualPing = [NSDate timeIntervalSinceReferenceDate];
    [self pollAndUpdateUI];
}

- (void)restoreFanToCurve:(uint32_t)fanID {
    if (_conn == IO_OBJECT_NULL) return;
    struct UM790SetModeReq req = {
        .fan_id = fanID,
        .mode = UM790_MODE_CURVE,
        .rpm = 0,
        .preset_id = UM790_PRESET_CUSTOM,
    };
    IOConnectCallStructMethod(_conn, kUM790MethodSetMode, &req, sizeof(req), NULL, NULL);
    [self pollAndUpdateUI];
}

- (void)setupStatusBarAndMenu {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSStatusBarButton *btn = _statusItem.button;
    if (btn) {
        if (@available(macOS 11.0, *)) {
            NSImage *img = [NSImage imageWithSystemSymbolName:@"fanblades" accessibilityDescription:@"Fan"];
            if (img) {
                [img setTemplate:YES];
                btn.image = img;
                btn.imagePosition = NSImageLeading;
            }
        }
        btn.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightMedium];
        btn.title = @" --";
    }

    _menu = [[NSMenu alloc] initWithTitle:@"UM790Fan"];
    _menu.delegate = self;
    _menu.autoenablesItems = NO;

    _cpuInfoItem = [[NSMenuItem alloc] initWithTitle:@"CPU: 等待驱动..." action:nil keyEquivalent:@""];
    _sysInfoItem = [[NSMenuItem alloc] initWithTitle:@"系统: 等待驱动..." action:nil keyEquivalent:@""];
    _ecInfoItem  = [[NSMenuItem alloc] initWithTitle:@"状态: 未连接" action:nil keyEquivalent:@""];
    _cpuInfoItem.enabled = NO;
    _sysInfoItem.enabled = NO;
    _ecInfoItem.enabled  = NO;
    [_menu addItem:_cpuInfoItem];
    [_menu addItem:_sysInfoItem];
    [_menu addItem:_ecInfoItem];
    [_menu addItem:[NSMenuItem separatorItem]];

    _presetSilentItem = [[NSMenuItem alloc] initWithTitle:@"静音曲线（推荐）"
                                                   action:@selector(onPresetSilent:)
                                            keyEquivalent:@""];
    _presetBalancedItem = [[NSMenuItem alloc] initWithTitle:@"平衡曲线"
                                                     action:@selector(onPresetBalanced:)
                                              keyEquivalent:@""];
    _presetCoolItem = [[NSMenuItem alloc] initWithTitle:@"强冷曲线"
                                                 action:@selector(onPresetCool:)
                                          keyEquivalent:@""];
    _presetFirmwareItem = [[NSMenuItem alloc] initWithTitle:@"原厂固件自动"
                                                     action:@selector(onPresetFirmware:)
                                              keyEquivalent:@""];
    for (NSMenuItem *it in @[_presetSilentItem, _presetBalancedItem, _presetCoolItem, _presetFirmwareItem]) {
        it.target = self;
        [_menu addItem:it];
    }

    [_menu addItem:[NSMenuItem separatorItem]];

    _cpuManualMenu = [[NSMenu alloc] initWithTitle:@"手动定速 · CPU 风扇"];
    const int cpuRpms[] = { 0, 1000, 1400, 1800, 2200, 2600, 3000, 3600, 4200, 5100 };
    for (size_t i = 0; i < sizeof(cpuRpms) / sizeof(cpuRpms[0]); i++) {
        NSString *t = cpuRpms[i] == 0 ? @"停转 (0 RPM)" : [NSString stringWithFormat:@"%d RPM", cpuRpms[i]];
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:t action:@selector(onManualCpu:) keyEquivalent:@""];
        it.target = self;
        it.tag = cpuRpms[i];
        [_cpuManualMenu addItem:it];
    }
    [_cpuManualMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *cpuCurveBack = [[NSMenuItem alloc] initWithTitle:@"恢复温控曲线" action:@selector(onCpuBackToCurve:) keyEquivalent:@""];
    cpuCurveBack.target = self;
    [_cpuManualMenu addItem:cpuCurveBack];

    _cpuManualParent = [[NSMenuItem alloc] initWithTitle:@"手动定速 · CPU 风扇" action:nil keyEquivalent:@""];
    _cpuManualParent.submenu = _cpuManualMenu;
    [_menu addItem:_cpuManualParent];

    _sysManualMenu = [[NSMenu alloc] initWithTitle:@"手动定速 · 系统风扇"];
    const int sysRpms[] = { 0, 800, 1000, 1400, 1800, 2200, 2800, 3600, 5100 };
    for (size_t i = 0; i < sizeof(sysRpms) / sizeof(sysRpms[0]); i++) {
        NSString *t = sysRpms[i] == 0 ? @"停转 (0 RPM)" : [NSString stringWithFormat:@"%d RPM", sysRpms[i]];
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:t action:@selector(onManualSys:) keyEquivalent:@""];
        it.target = self;
        it.tag = sysRpms[i];
        [_sysManualMenu addItem:it];
    }
    [_sysManualMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *sysCurveBack = [[NSMenuItem alloc] initWithTitle:@"恢复温控曲线" action:@selector(onSysBackToCurve:) keyEquivalent:@""];
    sysCurveBack.target = self;
    [_sysManualMenu addItem:sysCurveBack];

    _sysManualParent = [[NSMenuItem alloc] initWithTitle:@"手动定速 · 系统风扇" action:nil keyEquivalent:@""];
    _sysManualParent.submenu = _sysManualMenu;
    [_menu addItem:_sysManualParent];

    _cpuCppcMenu = [[NSMenu alloc] initWithTitle:@"CPU 睿频 (CPPC)"];
    _cppcPerfItem = [[NSMenuItem alloc] initWithTitle:@"高性能 (EPP 0x00)" action:@selector(onCpuPerf:) keyEquivalent:@""];
    _cppcBalItem  = [[NSMenuItem alloc] initWithTitle:@"平衡性能 (EPP 0x20，推荐)" action:@selector(onCpuBal:) keyEquivalent:@""];
    _cppcEcoItem  = [[NSMenuItem alloc] initWithTitle:@"节能低温 (EPP 0xA0)" action:@selector(onCpuEco:) keyEquivalent:@""];
    for (NSMenuItem *it in @[_cppcPerfItem, _cppcBalItem, _cppcEcoItem]) {
        it.target = self;
        [_cpuCppcMenu addItem:it];
    }
    _cpuCppcParent = [[NSMenuItem alloc] initWithTitle:@"CPU 睿频 (CPPC)" action:nil keyEquivalent:@""];
    _cpuCppcParent.submenu = _cpuCppcMenu;
    [_menu addItem:_cpuCppcParent];

    [_menu addItem:[NSMenuItem separatorItem]];

    _watchToggleItem = [[NSMenuItem alloc] initWithTitle:@"驱动与性能后台检测"
                                                  action:@selector(onToggleWatch:)
                                           keyEquivalent:@""];
    _watchToggleItem.target = self;
    [_menu addItem:_watchToggleItem];

    _watchStatusItem = [[NSMenuItem alloc] initWithTitle:@"检测状态: 读取中..."
                                                  action:nil
                                           keyEquivalent:@""];
    _watchStatusItem.enabled = NO;
    [_menu addItem:_watchStatusItem];

    _watchMenu = [[NSMenu alloc] initWithTitle:@"后台检测详情与选项"];
    _watchNotifyItem = [[NSMenuItem alloc] initWithTitle:@"严重异常桌面弹窗通知"
                                                  action:@selector(onToggleWatchNotify:)
                                           keyEquivalent:@""];
    _watchNotifyItem.target = self;
    [_watchMenu addItem:_watchNotifyItem];

    _watchEventsAnchorSep = [NSMenuItem separatorItem];
    [_watchMenu addItem:_watchEventsAnchorSep];
    _watchEventItems = [[NSMutableArray alloc] init];

    [_watchMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *watchRepItem = [[NSMenuItem alloc] initWithTitle:@"生成并查看诊断报告"
                                                          action:@selector(onOpenWatchReport:)
                                                   keyEquivalent:@""];
    watchRepItem.target = self;
    NSMenuItem *watchAckItem = [[NSMenuItem alloc] initWithTitle:@"清空未读告警角标"
                                                          action:@selector(onAckWatch:)
                                                   keyEquivalent:@""];
    watchAckItem.target = self;
    NSMenuItem *watchLogsItem = [[NSMenuItem alloc] initWithTitle:@"打开检测日志目录"
                                                           action:@selector(onOpenWatchLogs:)
                                                    keyEquivalent:@""];
    watchLogsItem.target = self;
    [_watchMenu addItem:watchRepItem];
    [_watchMenu addItem:watchAckItem];
    [_watchMenu addItem:watchLogsItem];

    _watchParent = [[NSMenuItem alloc] initWithTitle:@"后台检测详情与选项" action:nil keyEquivalent:@""];
    _watchParent.submenu = _watchMenu;
    [_menu addItem:_watchParent];

    [_menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quitFw = [[NSMenuItem alloc] initWithTitle:@"退出并恢复原厂固件"
                                                   action:@selector(onQuitFirmware:)
                                            keyEquivalent:@""];
    quitFw.target = self;
    NSMenuItem *quitKeep = [[NSMenuItem alloc] initWithTitle:@"退出（保持当前曲线）"
                                                     action:@selector(onQuitKeep:)
                                              keyEquivalent:@"q"];
    quitKeep.target = self;
    [_menu addItem:quitFw];
    [_menu addItem:quitKeep];

    _statusItem.menu = _menu;
}

- (void)refreshWatchState {
    NSString *cfgPath = [@"~/Library/Application Support/UM790Watch/config.json" stringByExpandingTildeInPath];
    BOOL enabled = NO;
    BOOL notify = YES;
    NSData *cfgData = [NSData dataWithContentsOfFile:cfgPath];
    if (cfgData) {
        id obj = [NSJSONSerialization JSONObjectWithData:cfgData options:0 error:nil];
        if ([obj isKindOfClass:[NSDictionary class]]) {
            if ([obj[@"enabled"] respondsToSelector:@selector(boolValue)]) {
                enabled = [obj[@"enabled"] boolValue];
            }
            if ([obj[@"notify"] respondsToSelector:@selector(boolValue)]) {
                notify = [obj[@"notify"] boolValue];
            }
        }
    }

    BOOL running = NO;
    NSInteger unreadCrit = 0;
    NSInteger unreadWarn = 0;
    NSDictionary *metrics = nil;
    NSArray *recent = nil;

    NSData *stData = [NSData dataWithContentsOfFile:@"/tmp/um790watch-status.json"];
    if (stData) {
        id stObj = [NSJSONSerialization JSONObjectWithData:stData options:0 error:nil];
        if ([stObj isKindOfClass:[NSDictionary class]]) {
            pid_t pid = (pid_t)([stObj[@"pid"] respondsToSelector:@selector(intValue)] ? [stObj[@"pid"] intValue] : 0);
            BOOL stRun = [stObj[@"running"] respondsToSelector:@selector(boolValue)] && [stObj[@"running"] boolValue];
            BOOL pidAlive = (pid > 0 && kill(pid, 0) == 0);
            running = (stRun && pidAlive);
            if ([stObj[@"unread_critical"] respondsToSelector:@selector(integerValue)]) {
                unreadCrit = [stObj[@"unread_critical"] integerValue];
            }
            if ([stObj[@"unread_warn"] respondsToSelector:@selector(integerValue)]) {
                unreadWarn = [stObj[@"unread_warn"] integerValue];
            }
            if ([stObj[@"metrics"] isKindOfClass:[NSDictionary class]]) {
                metrics = stObj[@"metrics"];
            }
            if ([stObj[@"recent_events"] isKindOfClass:[NSArray class]]) {
                recent = stObj[@"recent_events"];
            }
        }
    }

    _watchEnabled = enabled;
    _watchRunning = running;
    _watchNotify = notify;
    _watchUnreadCritical = unreadCrit;
    _watchUnreadWarn = unreadWarn;
    _watchMetrics = metrics ?: @{};
    _watchRecentEvents = recent ?: @[];
}

- (void)updateWatchUI {
    _watchToggleItem.enabled = !_watchBusy;
    _watchToggleItem.state = (_watchEnabled && _watchRunning) ? NSControlStateValueOn : NSControlStateValueOff;
    _watchNotifyItem.state = _watchNotify ? NSControlStateValueOn : NSControlStateValueOff;

    if (!_watchEnabled && !_watchRunning) {
        _watchStatusItem.title = @"检测状态: 已关闭";
    } else if (!_watchRunning) {
        _watchStatusItem.title = @"检测状态: 已启用 (等待后台启动)";
    } else if (_watchUnreadCritical > 0 || _watchUnreadWarn > 0) {
        _watchStatusItem.title = [NSString stringWithFormat:@"检测状态: 运行中 · %ld 严重 / %ld 警告",
                                  (long)_watchUnreadCritical, (long)_watchUnreadWarn];
    } else {
        id wsObj = _watchMetrics[@"ws_cpu_pct"];
        id fpsObj = _watchMetrics[@"active_fps"];
        if ([wsObj isKindOfClass:[NSNumber class]] && [fpsObj isKindOfClass:[NSNumber class]]) {
            _watchStatusItem.title = [NSString stringWithFormat:@"检测状态: 运行中 · 正常 (WS %.1f%% · %.0f FPS)",
                                      [wsObj doubleValue], [fpsObj doubleValue]];
        } else {
            _watchStatusItem.title = @"检测状态: 运行中 · 正常";
        }
    }

    for (NSMenuItem *it in _watchEventItems) {
        [_watchMenu removeItem:it];
    }
    [_watchEventItems removeAllObjects];

    NSInteger insertIdx = [_watchMenu indexOfItem:_watchEventsAnchorSep] + 1;
    if (_watchRecentEvents.count == 0) {
        NSMenuItem *emptyIt = [[NSMenuItem alloc] initWithTitle:@"暂无异常记录" action:nil keyEquivalent:@""];
        emptyIt.enabled = NO;
        [_watchMenu insertItem:emptyIt atIndex:insertIdx];
        [_watchEventItems addObject:emptyIt];
    } else {
        NSUInteger limit = MIN((NSUInteger)5, _watchRecentEvents.count);
        for (NSUInteger i = 0; i < limit; i++) {
            NSDictionary *ev = _watchRecentEvents[i];
            if (![ev isKindOfClass:[NSDictionary class]]) continue;
            NSString *sev = [ev[@"severity"] isKindOfClass:[NSString class]] ? ev[@"severity"] : @"info";
            NSString *title = [ev[@"title"] isKindOfClass:[NSString class]] ? ev[@"title"] : @"异常事件";
            NSString *prefix = [sev isEqualToString:@"critical"] ? @"[严重]" : ([sev isEqualToString:@"warn"] ? @"[警告]" : @"[信息]");
            NSString *line = [NSString stringWithFormat:@"%@ %@", prefix, title];
            if (line.length > 44) {
                line = [[line substringToIndex:43] stringByAppendingString:@"…"];
            }
            NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:line action:nil keyEquivalent:@""];
            it.enabled = NO;
            [_watchMenu insertItem:it atIndex:insertIdx++];
            [_watchEventItems addObject:it];
        }
    }
}

- (void)runWatchCLI:(NSArray<NSString *> *)args then:(dispatch_block_t)done {
    if (_watchBusy) return;
    _watchBusy = YES;
    _watchToggleItem.enabled = NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *script = @"/usr/local/bin/um790watch";
        if (![[NSFileManager defaultManager] fileExistsAtPath:script]) {
            script = @"/Users/limy/hackintosh/um790-watch/um790watch.py";
        }
        NSMutableArray<NSString *> *fullArgs = [NSMutableArray arrayWithObject:script];
        [fullArgs addObjectsFromArray:args];
        @try {
            NSTask *task = [[NSTask alloc] init];
            task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/python3"];
            task.arguments = fullArgs;
            task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
            task.standardError = [NSFileHandle fileHandleWithNullDevice];
            [task launchAndReturnError:nil];
            [task waitUntilExit];
        } @catch (NSException *ex) {
            logLine(@"um790watch task failed: %@", ex.reason);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_watchBusy = NO;
            [self refreshWatchState];
            [self updateUI];
            if (done) done();
        });
    });
}

- (void)updateUI {
    [self updateWatchUI];
    NSStatusBarButton *btn = _statusItem.button;
    BOOL writable = _hasStatus && _status.admitted != 0;

    for (NSMenuItem *it in @[_presetSilentItem, _presetBalancedItem, _presetCoolItem, _presetFirmwareItem]) {
        it.enabled = writable;
        it.state = NSControlStateValueOff;
    }
    _cpuManualParent.enabled = writable;
    _sysManualParent.enabled = writable;

    BOOL cppcReady = _hasStatus && _hasCpuStatus && _cpuStatus.supported != 0;
    _cpuCppcParent.enabled = cppcReady;
    for (NSMenuItem *it in @[_cppcPerfItem, _cppcBalItem, _cppcEcoItem]) {
        it.enabled = cppcReady;
        it.state = NSControlStateValueOff;
    }
    if (_hasStatus && _cpuMethodUnsupported) {
        _cpuCppcParent.title = @"CPU 睿频 (CPPC · 重启后生效)";
    } else if (_hasCpuStatus && !_cpuStatus.supported) {
        _cpuCppcParent.title = @"CPU 睿频 (CPPC · 未启用)";
    } else {
        _cpuCppcParent.title = @"CPU 睿频 (CPPC)";
        if (_hasCpuStatus) {
            if (_cpuStatus.profile_id == UM790_CPU_PROFILE_PERF) _cppcPerfItem.state = NSControlStateValueOn;
            else if (_cpuStatus.profile_id == UM790_CPU_PROFILE_POWERSAVE) _cppcEcoItem.state = NSControlStateValueOn;
            else if (_cpuStatus.profile_id == UM790_CPU_PROFILE_BALANCED) _cppcBalItem.state = NSControlStateValueOn;
        }
    }

    if (!_hasStatus) {
        if (btn) btn.title = @" 待重启";
        _cpuInfoItem.title = @"驱动未加载（AuxKC 已更新）";
        _sysInfoItem.title = @"请重启 macOS 以激活 UM790Fan.kext";
        _ecInfoItem.title  = @"状态: 等待内核驱动启动";
        return;
    }

    if (_status.admitted == 0) {
        if (btn) btn.title = @" 只读";
        _cpuInfoItem.title = [NSString stringWithFormat:@"CPU: %u°C · %d RPM",
                              _status.temp_c[UM790_FAN_CPU], _status.rpm[UM790_FAN_CPU]];
        _sysInfoItem.title = [NSString stringWithFormat:@"系统: %u°C · %d RPM",
                              _status.temp_c[UM790_FAN_SYS], _status.rpm[UM790_FAN_SYS]];
        NSString *err = _status.admit_error[0]
            ? [NSString stringWithUTF8String:_status.admit_error]
            : @"未通过主机校验";
        _ecInfoItem.title = [NSString stringWithFormat:@"只读保护: %@", err];
        return;
    }

    if (btn) {
        NSString *badge = (_watchEnabled && _watchRunning && _watchUnreadCritical > 0) ? @" !" : @"";
        btn.title = [NSString stringWithFormat:@" %u° %d%@",
                     _status.temp_c[UM790_FAN_CPU], _status.rpm[UM790_FAN_CPU], badge];
    }

    _cpuInfoItem.title = [NSString stringWithFormat:@"CPU: %u°C · %d RPM · %@",
                          _status.temp_c[UM790_FAN_CPU],
                          _status.rpm[UM790_FAN_CPU],
                          modeLabel(_status.mode[UM790_FAN_CPU], _status.cur_code[UM790_FAN_CPU],
                                    _status.owned[UM790_FAN_CPU], _status.hot[UM790_FAN_CPU],
                                    _status.holding[UM790_FAN_CPU])];
    _sysInfoItem.title = [NSString stringWithFormat:@"系统: %u°C · %d RPM · %@",
                          _status.temp_c[UM790_FAN_SYS],
                          _status.rpm[UM790_FAN_SYS],
                          modeLabel(_status.mode[UM790_FAN_SYS], _status.cur_code[UM790_FAN_SYS],
                                    _status.owned[UM790_FAN_SYS], _status.hot[UM790_FAN_SYS],
                                    _status.holding[UM790_FAN_SYS])];
    _ecInfoItem.title = [NSString stringWithFormat:@"EC: IT5571 · BIOS 表: %s · 已校验",
                         _status.profile_name[0] ? _status.profile_name : "?"];

    if (_status.mode[UM790_FAN_CPU] == UM790_MODE_FIRMWARE &&
        _status.mode[UM790_FAN_SYS] == UM790_MODE_FIRMWARE) {
        _presetFirmwareItem.state = NSControlStateValueOn;
    } else if (_status.preset_id == UM790_PRESET_SILENT) {
        _presetSilentItem.state = NSControlStateValueOn;
    } else if (_status.preset_id == UM790_PRESET_BALANCED) {
        _presetBalancedItem.state = NSControlStateValueOn;
    } else if (_status.preset_id == UM790_PRESET_COOL) {
        _presetCoolItem.state = NSControlStateValueOn;
    }
}

- (void)pollAndUpdateUI {
    [self refreshWatchState];
    if ([self fetchStatus]) {
        if (!_appliedInitialConfig && _status.admitted) {
            _appliedInitialConfig = YES;
            if (_status.admitted) {
                logLine(@"Driver admitted (profile=%s); applying initial preset=%@",
                        _status.profile_name, presetKeyFromID(_desiredPreset));
                [self applyPreset:_desiredPreset persist:NO];
            }
            if (_hasCpuStatus && _cpuStatus.supported) {
                logLine(@"CPPC supported (epp=0x%02x); applying cpu_profile=%@",
                        _cpuStatus.epp, cpuProfileKeyFromID(_desiredCpuProfile));
                [self applyCpuProfile:_desiredCpuProfile persist:NO];
            }
            [self fetchStatus];
        }
        if (_status.admitted &&
            (_status.mode[UM790_FAN_CPU] == UM790_MODE_MANUAL ||
             _status.mode[UM790_FAN_SYS] == UM790_MODE_MANUAL)) {
            NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
            if (now - _lastManualPing >= 15.0) {
                struct UM790PingReq preq = { .fan_id = UM790_FAN_ALL };
                IOConnectCallStructMethod(_conn, kUM790MethodPingManual,
                                          &preq, sizeof(preq), NULL, NULL);
                _lastManualPing = now;
            }
        }
    }
    [self updateUI];
}

- (void)onPresetSilent:(id)sender { (void)sender; [self applyPreset:UM790_PRESET_SILENT persist:YES]; [self pollAndUpdateUI]; }
- (void)onPresetBalanced:(id)sender { (void)sender; [self applyPreset:UM790_PRESET_BALANCED persist:YES]; [self pollAndUpdateUI]; }
- (void)onPresetCool:(id)sender { (void)sender; [self applyPreset:UM790_PRESET_COOL persist:YES]; [self pollAndUpdateUI]; }
- (void)onPresetFirmware:(id)sender { (void)sender; [self applyPreset:UM790_PRESET_FIRMWARE persist:YES]; [self pollAndUpdateUI]; }

- (void)onCpuPerf:(id)sender { (void)sender; [self applyCpuProfile:UM790_CPU_PROFILE_PERF persist:YES]; [self pollAndUpdateUI]; }
- (void)onCpuBal:(id)sender  { (void)sender; [self applyCpuProfile:UM790_CPU_PROFILE_BALANCED persist:YES]; [self pollAndUpdateUI]; }
- (void)onCpuEco:(id)sender  { (void)sender; [self applyCpuProfile:UM790_CPU_PROFILE_POWERSAVE persist:YES]; [self pollAndUpdateUI]; }

- (void)onManualCpu:(NSMenuItem *)sender { [self applyManualFan:UM790_FAN_CPU rpm:(int32_t)sender.tag]; }
- (void)onManualSys:(NSMenuItem *)sender { [self applyManualFan:UM790_FAN_SYS rpm:(int32_t)sender.tag]; }
- (void)onCpuBackToCurve:(id)sender { (void)sender; [self restoreFanToCurve:UM790_FAN_CPU]; }
- (void)onSysBackToCurve:(id)sender { (void)sender; [self restoreFanToCurve:UM790_FAN_SYS]; }

- (void)onToggleWatch:(id)sender {
    (void)sender;
    NSString *cmd = (_watchEnabled && _watchRunning) ? @"off" : @"on";
    [self runWatchCLI:@[cmd] then:nil];
}

- (void)onToggleWatchNotify:(id)sender {
    (void)sender;
    NSString *state = _watchNotify ? @"off" : @"on";
    [self runWatchCLI:@[@"notify", state] then:nil];
}

- (void)onAckWatch:(id)sender {
    (void)sender;
    [self runWatchCLI:@[@"ack"] then:nil];
}

- (void)onOpenWatchReport:(id)sender {
    (void)sender;
    [self runWatchCLI:@[@"report", @"--save"] then:^{
        NSString *rep = [@"~/Library/Logs/um790-watch/latest-report.md" stringByExpandingTildeInPath];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:rep]];
    }];
}

- (void)onOpenWatchLogs:(id)sender {
    (void)sender;
    NSString *dir = [@"~/Library/Logs/um790-watch" stringByExpandingTildeInPath];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir]];
}

- (void)onQuitFirmware:(id)sender {
    (void)sender;
    if (_conn != IO_OBJECT_NULL && _hasStatus && _status.admitted) {
        [self applyPreset:UM790_PRESET_FIRMWARE persist:NO];
    }
    [NSApp terminate:nil];
}

- (void)onQuitKeep:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}

- (void)menuWillOpen:(NSMenu *)menu {
    (void)menu;
    [self pollAndUpdateUI];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [self loadSavedConfig];
    [self setupStatusBarAndMenu];
    [self pollAndUpdateUI];
    _pollTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                  target:self
                                                selector:@selector(pollAndUpdateUI)
                                                userInfo:nil
                                                 repeats:YES];
    logLine(@"UM790FanBar started (saved preset=%@)", presetKeyFromID(_desiredPreset));
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if (_conn != IO_OBJECT_NULL && _hasStatus && _status.admitted) {
        if (_status.mode[UM790_FAN_CPU] == UM790_MODE_MANUAL ||
            _status.mode[UM790_FAN_SYS] == UM790_MODE_MANUAL) {
            [self applyPreset:_desiredPreset persist:NO];
        }
    }
    [self closeConn];
}

@end

static void handleTermSignal(int sig) {
    (void)sig;
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;
    signal(SIGTERM, handleTermSignal);
    signal(SIGINT, handleTermSignal);
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        UM790FanBarApp *delegate = [[UM790FanBarApp alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
