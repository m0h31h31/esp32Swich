#include "io/RelayController.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include <Preferences.h>

static const char* TAG = "RELAY";

static const char* srcName(RelaySource s) {
    switch (s) {
        case RS_BOOT:     return "上电";
        case RS_BUTTON:   return "按键";
        case RS_WEB:      return "网页";
        case RS_MQTT:     return "巴法云";
        case RS_SCHEDULE: return "定时";
        default:          return "自动";
    }
}

RelayController& RelayController::instance() {
    static RelayController inst;
    return inst;
}

// ---------------------------------------------------------------------------
// 断电前状态持久化（powerOn=2 时使用）
// ---------------------------------------------------------------------------
void RelayController::saveState() {
    Preferences p;
    if (!p.begin(NVS_NS_STATE, false)) return;
    p.putBool("relay", _state);
    p.end();
}

bool RelayController::loadSavedState() {
    Preferences p;
    if (!p.begin(NVS_NS_STATE, true)) return false;
    bool s = p.getBool("relay", false);
    p.end();
    return s;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void RelayController::begin() {
    const RelayConfig& rc = CFG.relay;
    _pin        = rc.pin;
    _activeHigh = rc.activeHigh;

    // 关键：先把引脚设为"断开"电平再设为输出，避免上电瞬间误吸合
    digitalWrite(_pin, _activeHigh ? LOW : HIGH);
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, _activeHigh ? LOW : HIGH);

    bool initial = false;
    switch (rc.powerOn) {
        case 0: initial = false; break;
        case 1: initial = true;  break;
        case 2: initial = loadSavedState(); break;
    }

    _state        = initial;
    _switchCount  = 0;
    _onAccumMs    = 0;
    _lastChangeMs = millis();
    _inited       = true;
    writeHardware(_state);
    if (_state) _onStartMs = millis();

    LOG_I(TAG, "继电器初始化: IO%d, %s有效, 上电策略=%s, 初始=%s",
          _pin, _activeHigh ? "高电平" : "低电平",
          rc.powerOn == 0 ? "关" : (rc.powerOn == 1 ? "开" : "恢复断电前"),
          _state ? "开" : "关");

    if (_cb) _cb(_state, RS_BOOT);
    if (_state && CFG.relay.autoOffS) _autoOffAtMs = millis() + CFG.relay.autoOffS * 1000UL;
}

void RelayController::applyConfig() {
    const RelayConfig& rc = CFG.relay;
    if (rc.pin == _pin && rc.activeHigh == _activeHigh) return;

    // 换引脚：先释放旧引脚（置为输入高阻，避免旧引脚保持吸合）
    if (_pin >= 0 && rc.pin != _pin) {
        digitalWrite(_pin, _activeHigh ? LOW : HIGH);
        pinMode(_pin, INPUT);
        LOG_W(TAG, "继电器引脚变更 IO%d -> IO%d", _pin, rc.pin);
    }
    if (rc.activeHigh != _activeHigh)
        LOG_W(TAG, "继电器触发电平变更: %s -> %s",
              _activeHigh ? "高" : "低", rc.activeHigh ? "高" : "低");

    _pin        = rc.pin;
    _activeHigh = rc.activeHigh;
    pinMode(_pin, OUTPUT);
    writeHardware(_state);   // 保持逻辑状态不变，按新极性重新输出
}

void RelayController::writeHardware(bool on) {
    if (_pin < 0) return;
    digitalWrite(_pin, (on == _activeHigh) ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// 控制
// ---------------------------------------------------------------------------
bool RelayController::set(bool on, RelaySource src, bool force) {
    if (!_inited) return false;

    if (on == _state) {
        // 状态未变，但仍刷新自动关闭计时（例如重复下发"开"）
        if (on && CFG.relay.autoOffS) _autoOffAtMs = millis() + CFG.relay.autoOffS * 1000UL;
        return true;
    }

    uint32_t now = millis();
    if (!force && CFG.relay.minIntervalMs &&
        (uint32_t)(now - _lastChangeMs) < CFG.relay.minIntervalMs) {
        LOG_W(TAG, "动作过于频繁，已忽略（间隔 <%ums）", CFG.relay.minIntervalMs);
        return false;
    }

    // 统计导通时长
    if (_state && _onStartMs) _onAccumMs += (now - _onStartMs);

    _state = on;
    writeHardware(_state);
    _lastChangeMs = now;
    _switchCount++;
    _onStartMs = _state ? now : 0;

    // 自动关闭计时
    _autoOffAtMs = (_state && CFG.relay.autoOffS) ? now + CFG.relay.autoOffS * 1000UL : 0;

    if (CFG.relay.powerOn == 2) saveState();

    LOG_I(TAG, "继电器 -> %s (来源: %s)", _state ? "开" : "关", srcName(src));
    if (_cb) _cb(_state, src);
    return true;
}

bool RelayController::toggle(RelaySource src) {
    return set(!_state, src);
}

void RelayController::tick() {
    if (!_inited) return;
    if (_autoOffAtMs && (int32_t)(millis() - _autoOffAtMs) >= 0) {
        _autoOffAtMs = 0;
        LOG_I(TAG, "达到自动关闭时间 (%us)", CFG.relay.autoOffS);
        set(false, RS_AUTO, true);
    }
}

uint32_t RelayController::onSeconds() {
    uint32_t total = _onAccumMs;
    if (_state && _onStartMs) total += (millis() - _onStartMs);
    return total / 1000;
}

void RelayController::toJson(JsonObject root) {
    root["state"]       = _state;
    root["pin"]         = _pin;
    root["activeHigh"]  = _activeHigh;
    root["switchCount"] = _switchCount;
    root["onSeconds"]   = onSeconds();
    root["sinceChange"] = (millis() - _lastChangeMs) / 1000;
    root["autoOffIn"]   = _autoOffAtMs ? (uint32_t)((_autoOffAtMs - millis()) / 1000) : 0;
}

// ---------------------------------------------------------------------------
// 引脚探测（不确定继电器接哪个 IO 时使用）
// ---------------------------------------------------------------------------
bool RelayController::probePin(int pin, uint16_t pulseMs, bool activeHigh) {
    // 屏蔽会导致复位/启动异常的引脚
    static const int kBlocked[] = {0, 1, 3, 6, 7, 8, 9, 10, 11, 20, 24, 28, 29, 30, 31};
    for (int b : kBlocked)
        if (pin == b) { LOG_W(TAG, "IO%d 为保留引脚，禁止探测", pin); return false; }
    if (pin < 0 || pin > 39) return false;
    if (pulseMs > 3000) pulseMs = 3000;

    LOG_I(TAG, "探测 IO%d：输出%s电平 %ums", pin, activeHigh ? "高" : "低", pulseMs);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, activeHigh ? HIGH : LOW);
    delay(pulseMs);
    digitalWrite(pin, activeHigh ? LOW : HIGH);

    // 探测结束后恢复真正的继电器输出，避免误改状态
    RelayController& r = instance();
    if (pin != r._pin) pinMode(pin, INPUT);
    else               r.writeHardware(r._state);
    return true;
}
