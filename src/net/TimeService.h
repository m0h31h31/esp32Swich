/**
 * @file    TimeService.h
 * @brief   NTP 联网校时 + 时区管理
 *
 * 说明：使用 configTzTime() 交给 lwIP SNTP 后台维护，不阻塞主循环；
 *       断网期间依靠 RTC 继续走时，联网后自动重新同步。
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

class TimeService {
public:
    static TimeService& instance();

    void begin();
    void tick();

    /// 网络恢复时调用，触发一次立即同步
    void requestSync();

    bool   isSynced() const { return _synced; }
    time_t now() const { return time(nullptr); }

    /// 格式化当前本地时间，如 2026-09-12 15:04:33
    String nowString(const char* fmt = "%Y-%m-%d %H:%M:%S") const;
    /// 上次成功同步距今秒数，未同步返回 0
    uint32_t sinceLastSync() const;

    void applyConfig();
    void toJson(JsonObject root);

private:
    TimeService() = default;
    void startSntp();

    bool     _synced        = false;
    bool     _started       = false;
    uint32_t _lastSyncMs    = 0;
    uint32_t _lastAttemptMs = 0;
    uint32_t _syncCount     = 0;
    bool     _pendingSync   = false;
};
