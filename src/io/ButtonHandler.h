/**
 * @file    ButtonHandler.h
 * @brief   IO0 按键处理：消抖 + 短按/双击/三击/长按 状态机（全程非阻塞）
 *
 * 功能映射（需求 10 + 扩展）：
 *   单击      -> 切换继电器
 *   双击      -> 开启 AP 配网 + 蓝牙广播（应急入口）
 *   三击      -> 重启设备
 *   长按 ≥10s -> 恢复出厂设置
 */
#pragma once

#include <Arduino.h>
#include <functional>

class ButtonHandler {
public:
    using VoidCb     = std::function<void()>;
    using ProgressCb = std::function<void(uint8_t percent)>;

    static ButtonHandler& instance();

    void begin();
    void applyConfig();
    void tick();

    void onSingleClick(VoidCb cb) { _onSingle  = cb; }
    void onDoubleClick(VoidCb cb) { _onDouble  = cb; }
    void onTripleClick(VoidCb cb) { _onTriple  = cb; }
    void onLongPress(VoidCb cb)   { _onLong    = cb; }
    /// 长按进行中的进度回调（0~100），用于 LED/日志提示
    void onLongProgress(ProgressCb cb) { _onProgress = cb; }

    bool    isPressed() const { return _pressed; }
    uint8_t longProgress() const;   ///< 当前长按进度百分比，未按下为 0

private:
    ButtonHandler() = default;
    bool readRaw();

    int8_t   _pin        = -1;
    bool     _activeLow  = true;
    bool     _enabled    = false;

    bool     _pressed    = false;   ///< 去抖后的稳定状态
    bool     _lastRaw    = false;
    uint32_t _lastEdgeMs = 0;       ///< 上次电平跳变时刻（消抖用）
    uint32_t _pressMs    = 0;       ///< 本次按下起始时刻
    uint32_t _releaseMs  = 0;       ///< 上次抬起时刻
    uint8_t  _clickCount = 0;
    bool     _longFired  = false;   ///< 本次按下是否已触发长按
    uint8_t  _lastProgress = 0;

    VoidCb     _onSingle   = nullptr;
    VoidCb     _onDouble   = nullptr;
    VoidCb     _onTriple   = nullptr;
    VoidCb     _onLong     = nullptr;
    ProgressCb _onProgress = nullptr;
};
