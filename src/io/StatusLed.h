/**
 * @file    StatusLed.h
 * @brief   状态指示灯：用不同闪烁节奏表达设备状态（可选，未接灯时自动禁用）
 */
#pragma once

#include <Arduino.h>

/// 指示灯模式
enum LedMode : uint8_t {
    LM_OFF = 0,      ///< 常灭
    LM_ON,           ///< 常亮（已联网且 MQTT 正常）
    LM_SLOW_BLINK,   ///< 慢闪 1Hz：正在连接 WiFi
    LM_FAST_BLINK,   ///< 快闪 5Hz：配网模式 / AP 模式
    LM_DOUBLE_BLINK, ///< 双闪：已联网但 MQTT 未连接
    LM_HEARTBEAT,    ///< 心跳：一切正常的低调提示
};

class StatusLed {
public:
    static StatusLed& instance();

    void begin();
    void applyConfig();
    void tick();

    void setMode(LedMode mode);
    LedMode mode() const { return _mode; }

    /// 临时闪烁 n 次后恢复原模式（用于"操作已收到"的反馈）
    void blink(uint8_t times, uint16_t onMs = 80, uint16_t offMs = 80);

private:
    StatusLed() = default;
    void write(bool on);

    int8_t   _pin        = -1;
    bool     _activeHigh = true;
    bool     _enabled    = false;
    LedMode  _mode       = LM_OFF;
    bool     _level      = false;
    uint32_t _lastMs     = 0;
    uint8_t  _phase      = 0;

    uint8_t  _blinkLeft  = 0;
    uint16_t _blinkOnMs  = 80;
    uint16_t _blinkOffMs = 80;
    LedMode  _savedMode  = LM_OFF;
};
