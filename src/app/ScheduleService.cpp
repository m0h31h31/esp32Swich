#include "app/ScheduleService.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "net/TimeService.h"
#include "io/RelayController.h"
#include <time.h>

static const char* TAG = "SCHED";
static const char* kWeekNames[7] = {"日", "一", "二", "三", "四", "五", "六"};

ScheduleService& ScheduleService::instance() {
    static ScheduleService inst;
    return inst;
}

void ScheduleService::begin() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++)
        if (CFG.schedules[i].enable) n++;
    if (n) LOG_I(TAG, "已启用 %u 条定时任务", n);
}

void ScheduleService::tick() {
    if (!TimeService::instance().isSynced()) return;

    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);

    // 以"自年初的分钟数"作为去重键，跨天/跨小时都不会重复触发
    uint32_t minuteKey = (uint32_t)tmv.tm_yday * 1440 + tmv.tm_hour * 60 + tmv.tm_min;
    if (minuteKey == _lastCheckMin) return;      // 同一分钟内只检查一次
    _lastCheckMin = minuteKey;

    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        const ScheduleItem& s = CFG.schedules[i];
        if (!s.enable) continue;
        if (!(s.days & (1 << tmv.tm_wday))) continue;      // 今天不在生效日
        if (s.hour != tmv.tm_hour || s.minute != tmv.tm_min) continue;
        if (_lastFired[i] == minuteKey) continue;          // 已触发过

        _lastFired[i] = minuteKey;
        _firedCount++;

        RelayController& r = RelayController::instance();
        const char* act = "翻转";
        switch (s.action) {
            case 0: r.set(false, RS_SCHEDULE, true); act = "关闭"; break;
            case 1: r.set(true,  RS_SCHEDULE, true); act = "开启"; break;
            default: r.toggle(RS_SCHEDULE); break;
        }
        LOG_I(TAG, "定时任务 #%u 触发：周%s %02u:%02u -> %s",
              i + 1, kWeekNames[tmv.tm_wday], s.hour, s.minute, act);
    }
}

int32_t ScheduleService::secondsToNext(String& desc) {
    if (!TimeService::instance().isSynced()) { desc = "等待校时"; return -1; }

    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int32_t nowSec = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;

    int32_t best = -1;
    int8_t  bestIdx = -1;
    int8_t  bestDay = 0;

    // 检查未来 7 天，找最近的一次
    for (int8_t d = 0; d < 8; d++) {
        int wday = (tmv.tm_wday + d) % 7;
        for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
            const ScheduleItem& s = CFG.schedules[i];
            if (!s.enable || !(s.days & (1 << wday))) continue;
            int32_t target = d * 86400 + s.hour * 3600 + s.minute * 60;
            int32_t delta  = target - nowSec;
            if (delta <= 0) continue;
            if (best < 0 || delta < best) { best = delta; bestIdx = i; bestDay = d; }
        }
        if (best >= 0 && d >= 1) break;   // 已找到今天或明天的任务即可
    }

    if (best < 0) { desc = "无"; return -1; }

    const ScheduleItem& s = CFG.schedules[bestIdx];
    const char* act = s.action == 0 ? "关闭" : (s.action == 1 ? "开启" : "翻转");
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %02u:%02u %s",
             bestDay == 0 ? "今天" : (bestDay == 1 ? "明天" : "稍后"),
             s.hour, s.minute, act);
    desc = buf;
    return best;
}

void ScheduleService::toJson(JsonArray arr) {
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        const ScheduleItem& s = CFG.schedules[i];
        JsonObject o = arr.add<JsonObject>();
        o["index"]  = i;
        o["enable"] = s.enable;
        o["days"]   = s.days;
        o["hour"]   = s.hour;
        o["minute"] = s.minute;
        o["action"] = s.action;
    }
}
