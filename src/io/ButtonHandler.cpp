#include "io/ButtonHandler.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "app_config.h"

static const char* TAG = "BTN";

ButtonHandler& ButtonHandler::instance() {
    static ButtonHandler inst;
    return inst;
}

void ButtonHandler::begin() {
    applyConfig();
    if (_enabled)
        LOG_I(TAG, "按键初始化: IO%d, %s有效, 长按 %ums 恢复出厂",
              _pin, _activeLow ? "低电平" : "高电平", CFG.button.longPressMs);
}

void ButtonHandler::applyConfig() {
    const ButtonConfig& bc = CFG.button;
    _enabled   = bc.enable && bc.pin >= 0;
    _pin       = bc.pin;
    _activeLow = bc.activeLow;
    if (!_enabled) return;

    // IO0 外部已有上拉（启动引脚），仍显式配置上/下拉提高抗干扰
    pinMode(_pin, _activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    _pressed    = false;
    _lastRaw    = false;
    _clickCount = 0;
    _longFired  = false;
}

bool ButtonHandler::readRaw() {
    int v = digitalRead(_pin);
    return _activeLow ? (v == LOW) : (v == HIGH);
}

uint8_t ButtonHandler::longProgress() const {
    if (!_pressed || _longFired) return 0;
    uint32_t held = millis() - _pressMs;
    uint32_t need = CFG.button.longPressMs;
    if (!need) return 0;
    uint32_t pct = held * 100 / need;
    return pct > 100 ? 100 : (uint8_t)pct;
}

void ButtonHandler::tick() {
    if (!_enabled) return;

    uint32_t now = millis();
    bool raw = readRaw();

    // ---- 消抖：电平持续稳定超过 debounceMs 才认可 ----
    if (raw != _lastRaw) {
        _lastRaw    = raw;
        _lastEdgeMs = now;
    } else if (raw != _pressed && (now - _lastEdgeMs) >= CFG.button.debounceMs) {
        _pressed = raw;
        if (_pressed) {
            // 按下
            _pressMs      = now;
            _longFired    = false;
            _lastProgress = 0;
        } else {
            // 抬起
            _releaseMs = now;
            if (!_longFired) {
                _clickCount++;
                if (!CFG.button.multiClick) {
                    // 未启用多击时立即响应单击，手感更跟手
                    if (_onSingle) _onSingle();
                    _clickCount = 0;
                }
            } else {
                _clickCount = 0;     // 长按已处理，抬起不再计入点击
            }
            if (_onProgress && _lastProgress) { _onProgress(0); _lastProgress = 0; }
        }
    }

    // ---- 长按检测（按住期间即触发，无需抬手）----
    if (_pressed && !_longFired) {
        uint32_t held = now - _pressMs;
        if (_onProgress) {
            uint8_t p = longProgress();
            if (p / 10 != _lastProgress / 10) {     // 每 10% 回调一次，避免刷屏
                _lastProgress = p;
                _onProgress(p);
                if (p >= 30 && p < 100 && p % 30 == 0)
                    LOG_W(TAG, "长按中… %u%%，松手取消，满 %us 将恢复出厂设置",
                          p, CFG.button.longPressMs / 1000);
            }
        }
        if (held >= CFG.button.longPressMs) {
            _longFired  = true;
            _clickCount = 0;
            LOG_W(TAG, "检测到长按 %us —— 触发恢复出厂设置", held / 1000);
            if (_onLong) _onLong();
        }
    }

    // ---- 多击判定：抬起后超过间隔无新点击则结算 ----
    if (CFG.button.multiClick && _clickCount && !_pressed &&
        (now - _releaseMs) >= BUTTON_MULTI_GAP_MS) {
        uint8_t n = _clickCount;
        _clickCount = 0;
        switch (n) {
            case 1:
                LOG_I(TAG, "单击 -> 切换继电器");
                if (_onSingle) _onSingle();
                break;
            case 2:
                LOG_I(TAG, "双击 -> 开启配网模式");
                if (_onDouble) _onDouble();
                break;
            default:
                LOG_I(TAG, "%u 击 -> 重启设备", n);
                if (_onTriple) _onTriple();
                break;
        }
    }
}
