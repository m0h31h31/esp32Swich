#include "net/TimeService.h"
#include "net/WiFiService.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"

static const char* TAG = "TIME";

TimeService& TimeService::instance() {
    static TimeService inst;
    return inst;
}

void TimeService::begin() {
    // 先设时区，即使尚未联网也能保证之后同步到的时间显示正确
    setenv("TZ", CFG.time.tz.c_str(), 1);
    tzset();
    LOG_I(TAG, "时区设置为 %s", CFG.time.tz.c_str());
    _pendingSync = CFG.time.enable;
}

void TimeService::applyConfig() {
    setenv("TZ", CFG.time.tz.c_str(), 1);
    tzset();
    _started = false;          // 强制重新配置 SNTP 服务器
    _pendingSync = CFG.time.enable;
}

void TimeService::startSntp() {
    const TimeConfig& tc = CFG.time;
    configTzTime(tc.tz.c_str(),
                 tc.ntp1.length() ? tc.ntp1.c_str() : DEF_NTP1,
                 tc.ntp2.length() ? tc.ntp2.c_str() : nullptr,
                 tc.ntp3.length() ? tc.ntp3.c_str() : nullptr);
    _started       = true;
    _lastAttemptMs = millis();
    LOG_I(TAG, "开始 NTP 校时 (%s)", tc.ntp1.c_str());
}

void TimeService::requestSync() {
    if (!CFG.time.enable) return;
    _pendingSync = true;
}

void TimeService::tick() {
    if (!CFG.time.enable) return;
    if (!WiFiService::instance().staOnline()) return;

    uint32_t now = millis();

    // 触发同步：首次联网、手动请求、或到达周期
    if (_pendingSync || !_started ||
        (_synced && (now - _lastSyncMs) > CFG.time.intervalS * 1000UL) ||
        (!_synced && (now - _lastAttemptMs) > 30000UL)) {
        _pendingSync = false;
        startSntp();
    }

    // 检测同步是否完成（时间戳越过 2020-01-01 视为有效）
    if (_started) {
        time_t t = time(nullptr);
        if (t > 1577808000) {
            if (!_synced || (now - _lastSyncMs) > CFG.time.intervalS * 1000UL) {
                bool first = !_synced;
                _synced     = true;
                _lastSyncMs = now;
                _syncCount++;
                if (first || _syncCount <= 2 || _syncCount % 24 == 0)
                    LOG_I(TAG, "时间同步成功: %s", nowString().c_str());
            }
        }
    }
}

String TimeService::nowString(const char* fmt) const {
    time_t t = time(nullptr);
    if (t < 1577808000) return "未同步";
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[40];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return String(buf);
}

uint32_t TimeService::sinceLastSync() const {
    if (!_synced) return 0;
    return (millis() - _lastSyncMs) / 1000;
}

void TimeService::toJson(JsonObject root) {
    root["enable"]    = CFG.time.enable;
    root["synced"]    = _synced;
    root["now"]       = nowString();
    root["date"]      = nowString("%Y-%m-%d");
    root["clock"]     = nowString("%H:%M:%S");
    root["weekday"]   = nowString("%A");
    root["epoch"]     = (uint32_t)time(nullptr);
    root["tz"]        = CFG.time.tz;
    root["syncCount"] = _syncCount;
    root["sinceSync"] = sinceLastSync();
    root["server"]    = CFG.time.ntp1;
}
