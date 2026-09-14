/**
 * @file    RelayController.h
 * @brief   继电器控制：电平极性可配、上电状态策略、动作节流、运行统计
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

/// 触发来源，用于日志与状态上报（避免 MQTT 回环）
enum RelaySource : uint8_t {
    RS_BOOT     = 0,   ///< 上电初始化
    RS_BUTTON   = 1,   ///< 物理按键
    RS_WEB      = 2,   ///< 网页
    RS_MQTT     = 3,   ///< 巴法云下发
    RS_SCHEDULE = 4,   ///< 定时任务
    RS_AUTO     = 5,   ///< 自动关闭 / 内部逻辑
};

class RelayController {
public:
    using ChangeCb = std::function<void(bool state, RelaySource src)>;

    static RelayController& instance();

    /// 按当前配置初始化引脚并应用上电策略
    void begin();

    /// 配置变化后重新应用（换引脚 / 换极性时保持逻辑状态不变）
    void applyConfig();

    /// 每次主循环调用，处理自动关闭等定时逻辑
    void tick();

    /// 设置状态；force=true 时忽略最小间隔限制
    bool set(bool on, RelaySource src, bool force = false);
    bool toggle(RelaySource src);

    bool     state() const { return _state; }
    uint32_t switchCount() const { return _switchCount; }
    uint32_t onSeconds();                 ///< 本次开机以来累计导通秒数
    uint32_t lastChangeMs() const { return _lastChangeMs; }

    /// 状态变化回调（用于 MQTT 上报 / LED 跟随）
    void onChange(ChangeCb cb) { _cb = cb; }

    void toJson(JsonObject root);

    /**
     * 引脚探测：临时在指定引脚输出一次脉冲，用于确认继电器接在哪个 IO
     * @param pin       待测引脚
     * @param pulseMs   脉冲时长
     * @param activeHigh 触发电平
     */
    static bool probePin(int pin, uint16_t pulseMs, bool activeHigh);

private:
    RelayController() = default;

    void writeHardware(bool on);
    void saveState();
    bool loadSavedState();

    int8_t   _pin         = -1;
    bool     _activeHigh  = true;
    bool     _state       = false;
    uint32_t _lastChangeMs= 0;
    uint32_t _switchCount = 0;
    uint32_t _onAccumMs   = 0;      ///< 已累计的导通毫秒
    uint32_t _onStartMs   = 0;      ///< 本次导通起始时刻
    uint32_t _autoOffAtMs = 0;      ///< 自动关闭时刻，0=不启用
    ChangeCb _cb          = nullptr;
    bool     _inited      = false;
};
