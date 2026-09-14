/**
 * @file    SysInfo.h
 * @brief   设备信息采集：芯片型号、温度、内存、Flash、分区、复位原因等
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class SysInfo {
public:
    /// 记录启动时刻，采集复位原因
    static void begin();

    /// 静态信息（芯片、固件、分区）——不会变化，网页只需拉一次
    static void toJsonStatic(JsonObject root);

    /// 动态信息（内存、温度、运行时长）——网页周期刷新
    static void toJsonRuntime(JsonObject root);

    static float    cpuTemperature();      ///< 片内温度（℃，ESP32 为参考值）
    static uint32_t uptimeSeconds();
    static String   uptimeString();
    static String   chipModel();
    static String   macAddress();
    static const char* resetReasonText();
    static String   formatBytes(uint64_t bytes);

    /// 堆内存健康度：0~100，越低越危险
    static uint8_t  heapHealth();

private:
    static uint32_t _bootMs;
    static int      _resetReason;
};
