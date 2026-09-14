#include "io/StatusLed.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"

StatusLed& StatusLed::instance() {
    static StatusLed inst;
    return inst;
}

void StatusLed::begin() { applyConfig(); }

void StatusLed::applyConfig() {
    const LedConfig& lc = CFG.led;
    bool wasEnabled = _enabled;
    if (wasEnabled && _pin >= 0 && (_pin != lc.pin || !lc.enable)) {
        write(false);
        pinMode(_pin, INPUT);
    }
    _enabled    = lc.enable && lc.pin >= 0;
    _pin        = lc.pin;
    _activeHigh = lc.activeHigh;
    if (_enabled) {
        pinMode(_pin, OUTPUT);
        write(false);
        LOG_I("LED", "指示灯启用: IO%d (%s有效)", _pin, _activeHigh ? "高" : "低");
    }
}

void StatusLed::write(bool on) {
    if (_pin < 0) return;
    _level = on;
    digitalWrite(_pin, (on == _activeHigh) ? HIGH : LOW);
}

void StatusLed::setMode(LedMode mode) {
    if (_mode == mode) return;
    _mode   = mode;
    _phase  = 0;
    _lastMs = millis();
    if (!_blinkLeft && _enabled) write(mode == LM_ON);
}

void StatusLed::blink(uint8_t times, uint16_t onMs, uint16_t offMs) {
    if (!_enabled || !times) return;
    if (!_blinkLeft) _savedMode = _mode;
    _blinkLeft  = times * 2;   // 每次闪烁包含亮/灭两个相位
    _blinkOnMs  = onMs;
    _blinkOffMs = offMs;
    _lastMs     = millis();
    write(true);
    _blinkLeft--;
}

void StatusLed::tick() {
    if (!_enabled) return;
    uint32_t now = millis();

    // ---- 临时闪烁优先 ----
    if (_blinkLeft) {
        uint16_t dur = _level ? _blinkOnMs : _blinkOffMs;
        if (now - _lastMs >= dur) {
            _lastMs = now;
            write(!_level);
            _blinkLeft--;
            if (!_blinkLeft) {
                LedMode m = _savedMode;
                _mode = (LedMode)0xFF;   // 强制 setMode 重新应用
                setMode(m);
            }
        }
        return;
    }

    switch (_mode) {
        case LM_OFF:
            if (_level) write(false);
            break;
        case LM_ON:
            if (!_level) write(true);
            break;
        case LM_SLOW_BLINK:
            if (now - _lastMs >= 500) { _lastMs = now; write(!_level); }
            break;
        case LM_FAST_BLINK:
            if (now - _lastMs >= 100) { _lastMs = now; write(!_level); }
            break;
        case LM_DOUBLE_BLINK: {
            // 亮100-灭150-亮100-灭1200 循环
            static const uint16_t seq[4] = {100, 150, 100, 1200};
            if (now - _lastMs >= seq[_phase]) {
                _lastMs = now;
                _phase  = (_phase + 1) & 0x03;
                write(_phase == 0 || _phase == 2);
            }
            break;
        }
        case LM_HEARTBEAT:
            // 短亮长灭，功耗低且明显
            if (_level && now - _lastMs >= 60)        { _lastMs = now; write(false); }
            else if (!_level && now - _lastMs >= 2000){ _lastMs = now; write(true); }
            break;
    }
}
