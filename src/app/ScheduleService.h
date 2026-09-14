/**
 * @file    ScheduleService.h
 * @brief   定时任务：按星期 + 时分执行 开/关/翻转
 *
 * 只有在 NTP 校时成功后才会执行，避免时间不准导致误动作；
 * 每个任务每分钟最多触发一次（记录上次触发的"年内分钟数"去重）。
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "app_config.h"

class ScheduleService {
public:
    static ScheduleService& instance();

    void begin();
    void tick();

    /// 距离下一次任务还有多少秒，无任务返回 -1
    int32_t secondsToNext(String& desc);

    void toJson(JsonArray arr);

private:
    ScheduleService() = default;

    uint32_t _lastFired[MAX_SCHEDULES] = {0};   ///< 上次触发的分钟序号
    uint32_t _lastCheckMin = 0xFFFFFFFF;
    uint32_t _firedCount   = 0;
};
