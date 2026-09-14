#include "core/ConfigManager.h"
#include <Preferences.h>
#include <WiFi.h>

static const char* TAG = "CFG";

// 读取辅助宏：JSON 中不存在该键时保持原值（实现"新增字段自动取默认"的向后兼容）
#define RD(j, k, v)                                                   \
    do {                                                              \
        JsonVariantConst _x = (j)[k];                                 \
        if (!_x.isNull()) v = _x.as<decltype(v)>();                   \
    } while (0)

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

// ---------------------------------------------------------------------------
// 基础信息
// ---------------------------------------------------------------------------
String ConfigManager::macSuffix() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X",
             (uint8_t)((mac >> 32) & 0xFF), (uint8_t)((mac >> 40) & 0xFF));
    return String(buf);
}

String ConfigManager::effectiveHostname() const {
    if (_cfg.sys.hostname.length()) return _cfg.sys.hostname;
    return "esp32sw-" + macSuffix();
}

String ConfigManager::effectiveApSsid() const {
    if (_cfg.wifi.ap.ssid.length()) return _cfg.wifi.ap.ssid;
    return "ESP32-Switch-" + macSuffix();
}

String ConfigManager::effectiveMqttClientId() const {
    // 巴法云以私钥作为 clientId；多设备共用私钥时追加后缀区分
    String id = _cfg.mqtt.key;
    if (_cfg.mqtt.clientSuffix.length()) id += _cfg.mqtt.clientSuffix;
    return id;
}

bool ConfigManager::isDefaultWebPassword() const {
    return _cfg.web.password == DEF_WEB_PASSWORD;
}

bool ConfigManager::hasWifiCredential() const {
    return _cfg.wifi.sta.ssid.length() > 0;
}

// ---------------------------------------------------------------------------
// 巴法云设备类型：主题名后三位数字决定设备类型
// ---------------------------------------------------------------------------
uint8_t ConfigManager::bafaTypeOf(const String& topic) {
    if (topic.length() < 3) return 0;
    String suf = topic.substring(topic.length() - 3);
    for (uint8_t i = 0; i < 3; i++)
        if (!isdigit((int)suf[i])) return 0;
    int code = suf.toInt();
    switch (code) {
        case 1: return 1;   // 插座/开关
        case 2: return 3;   // 灯
        case 3: return 5;   // 窗帘
        case 4: return 4;   // 传感器
        case 5: return 6;   // 空调
        case 6: return 7;   // 风扇
        case 7: return 8;   // 电视
        default: return 1;  // 其余按开关处理
    }
}

const char* ConfigManager::bafaTypeName(uint8_t type) {
    switch (type) {
        case 1: return "开关";
        case 2: return "插座";
        case 3: return "灯";
        case 4: return "传感器";
        case 5: return "窗帘";
        case 6: return "空调";
        case 7: return "风扇";
        case 8: return "电视";
        default: return "未知设备";
    }
}

// ---------------------------------------------------------------------------
// 默认值
// ---------------------------------------------------------------------------
void ConfigManager::setDefaults() {
    _cfg = AppConfig();                 // 结构体内的成员默认值即出厂值
    _cfg.version = CONFIG_VERSION;
    _cfg.mqtt.topicCount = 1;
    _cfg.mqtt.topics[0].enable = true;
    _cfg.mqtt.topics[0].topic  = DEF_MQTT_TOPIC;
    _cfg.mqtt.topics[0].name   = "主开关";
    _cfg.mqtt.topics[0].bind   = 1;
    _cfg.mqtt.topics[0].type   = bafaTypeOf(DEF_MQTT_TOPIC);
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------
void ConfigManager::toJson(JsonObject root, bool includeSecret) const {
    root["version"] = _cfg.version;

    JsonObject sys = root["sys"].to<JsonObject>();
    sys["deviceName"]   = _cfg.sys.deviceName;
    sys["hostname"]     = _cfg.sys.hostname;
    sys["logLevel"]     = _cfg.sys.logLevel;
    sys["autoRebootH"]  = _cfg.sys.autoRebootH;
    sys["mdns"]         = _cfg.sys.mdns;
    sys["wdtS"]         = _cfg.sys.wdtS;
    sys["otaUrl"]       = _cfg.sys.otaUrl;
    sys["otaAutoCheck"] = _cfg.sys.otaAutoCheck;

    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["mode"]        = _cfg.wifi.mode;
    wifi["apFallback"]  = _cfg.wifi.apFallback;
    wifi["apFallbackS"] = _cfg.wifi.apFallbackS;
    wifi["txPower"]     = _cfg.wifi.txPower;
    JsonObject sta = wifi["sta"].to<JsonObject>();
    sta["ssid"]     = _cfg.wifi.sta.ssid;
    sta["password"] = includeSecret ? _cfg.wifi.sta.password
                                    : (_cfg.wifi.sta.password.length() ? "********" : "");
    sta["dhcp"]     = _cfg.wifi.sta.dhcp;
    sta["ip"]       = _cfg.wifi.sta.ip;
    sta["gateway"]  = _cfg.wifi.sta.gateway;
    sta["netmask"]  = _cfg.wifi.sta.netmask;
    sta["dns1"]     = _cfg.wifi.sta.dns1;
    sta["dns2"]     = _cfg.wifi.sta.dns2;
    sta["bssidLock"]= _cfg.wifi.sta.bssidLock;
    sta["bssid"]    = _cfg.wifi.sta.bssid;
    JsonObject ap = wifi["ap"].to<JsonObject>();
    ap["ssid"]     = _cfg.wifi.ap.ssid;
    ap["password"] = includeSecret ? _cfg.wifi.ap.password
                                   : (_cfg.wifi.ap.password.length() ? "********" : "");
    ap["channel"]  = _cfg.wifi.ap.channel;
    ap["hidden"]   = _cfg.wifi.ap.hidden;
    ap["maxConn"]  = _cfg.wifi.ap.maxConn;
    ap["ip"]       = _cfg.wifi.ap.ip;

    JsonObject web = root["web"].to<JsonObject>();
    web["username"]   = _cfg.web.username;
    web["password"]   = includeSecret ? _cfg.web.password : "********";
    web["port"]       = _cfg.web.port;
    web["sessionMin"] = _cfg.web.sessionMin;
    web["allowLan"]   = _cfg.web.allowLan;

    JsonObject mqtt = root["mqtt"].to<JsonObject>();
    mqtt["enable"]          = _cfg.mqtt.enable;
    mqtt["host"]            = _cfg.mqtt.host;
    mqtt["port"]            = _cfg.mqtt.port;
    mqtt["key"]             = includeSecret ? _cfg.mqtt.key
                                            : (_cfg.mqtt.key.length() ? "********" : "");
    mqtt["clientSuffix"]    = _cfg.mqtt.clientSuffix;
    mqtt["keepalive"]       = _cfg.mqtt.keepalive;
    mqtt["reportOnChange"]  = _cfg.mqtt.reportOnChange;
    mqtt["reportIntervalS"] = _cfg.mqtt.reportIntervalS;
    mqtt["syncOnConnect"]   = _cfg.mqtt.syncOnConnect;
    mqtt["reportSuffix"]    = _cfg.mqtt.reportSuffix;
    JsonArray topics = mqtt["topics"].to<JsonArray>();
    for (uint8_t i = 0; i < _cfg.mqtt.topicCount && i < MAX_MQTT_TOPICS; i++) {
        JsonObject t = topics.add<JsonObject>();
        t["enable"] = _cfg.mqtt.topics[i].enable;
        t["topic"]  = _cfg.mqtt.topics[i].topic;
        t["name"]   = _cfg.mqtt.topics[i].name;
        t["bind"]   = _cfg.mqtt.topics[i].bind;
        t["type"]   = _cfg.mqtt.topics[i].type;
    }

    JsonObject relay = root["relay"].to<JsonObject>();
    relay["pin"]           = _cfg.relay.pin;
    relay["activeHigh"]    = _cfg.relay.activeHigh;
    relay["powerOn"]       = _cfg.relay.powerOn;
    relay["minIntervalMs"] = _cfg.relay.minIntervalMs;
    relay["autoOffS"]      = _cfg.relay.autoOffS;

    JsonObject btn = root["button"].to<JsonObject>();
    btn["enable"]      = _cfg.button.enable;
    btn["pin"]         = _cfg.button.pin;
    btn["activeLow"]   = _cfg.button.activeLow;
    btn["debounceMs"]  = _cfg.button.debounceMs;
    btn["longPressMs"] = _cfg.button.longPressMs;
    btn["multiClick"]  = _cfg.button.multiClick;

    JsonObject led = root["led"].to<JsonObject>();
    led["enable"]      = _cfg.led.enable;
    led["pin"]         = _cfg.led.pin;
    led["activeHigh"]  = _cfg.led.activeHigh;
    led["followRelay"] = _cfg.led.followRelay;

    JsonObject tm = root["time"].to<JsonObject>();
    tm["enable"]    = _cfg.time.enable;
    tm["ntp1"]      = _cfg.time.ntp1;
    tm["ntp2"]      = _cfg.time.ntp2;
    tm["ntp3"]      = _cfg.time.ntp3;
    tm["tz"]        = _cfg.time.tz;
    tm["intervalS"] = _cfg.time.intervalS;

    JsonArray sch = root["schedules"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        JsonObject s = sch.add<JsonObject>();
        s["enable"] = _cfg.schedules[i].enable;
        s["days"]   = _cfg.schedules[i].days;
        s["hour"]   = _cfg.schedules[i].hour;
        s["minute"] = _cfg.schedules[i].minute;
        s["action"] = _cfg.schedules[i].action;
    }
}

// ---------------------------------------------------------------------------
// 反序列化（增量）
// ---------------------------------------------------------------------------
bool ConfigManager::applyJson(JsonObjectConst root, String& err) {
    bool changed = false;

    if (JsonObjectConst j = root["sys"]) {
        RD(j, "deviceName",   _cfg.sys.deviceName);
        RD(j, "hostname",     _cfg.sys.hostname);
        RD(j, "logLevel",     _cfg.sys.logLevel);
        RD(j, "autoRebootH",  _cfg.sys.autoRebootH);
        RD(j, "mdns",         _cfg.sys.mdns);
        RD(j, "wdtS",         _cfg.sys.wdtS);
        RD(j, "otaUrl",       _cfg.sys.otaUrl);
        RD(j, "otaAutoCheck", _cfg.sys.otaAutoCheck);
        if (_cfg.sys.logLevel > LL_DEBUG) _cfg.sys.logLevel = LL_DEBUG;
        if (_cfg.sys.wdtS && _cfg.sys.wdtS < 10) _cfg.sys.wdtS = 10;
        changed = true;
    }

    if (JsonObjectConst j = root["wifi"]) {
        RD(j, "mode",        _cfg.wifi.mode);
        RD(j, "apFallback",  _cfg.wifi.apFallback);
        RD(j, "apFallbackS", _cfg.wifi.apFallbackS);
        RD(j, "txPower",     _cfg.wifi.txPower);
        if (_cfg.wifi.mode > WM_APSTA) _cfg.wifi.mode = WM_STA;
        if (JsonObjectConst s = j["sta"]) {
            RD(s, "ssid",      _cfg.wifi.sta.ssid);
            // 密码为掩码时表示"不修改"
            if (!s["password"].isNull()) {
                String p = s["password"].as<String>();
                if (p != "********") _cfg.wifi.sta.password = p;
            }
            RD(s, "dhcp",      _cfg.wifi.sta.dhcp);
            RD(s, "ip",        _cfg.wifi.sta.ip);
            RD(s, "gateway",   _cfg.wifi.sta.gateway);
            RD(s, "netmask",   _cfg.wifi.sta.netmask);
            RD(s, "dns1",      _cfg.wifi.sta.dns1);
            RD(s, "dns2",      _cfg.wifi.sta.dns2);
            RD(s, "bssidLock", _cfg.wifi.sta.bssidLock);
            RD(s, "bssid",     _cfg.wifi.sta.bssid);
            // 便利处理：用户填了 SSID 但没显式指定模式，且当前还停在纯热点模式时，
            // 自动切到 STA —— 否则填完 WiFi 却连不上，是很容易踩的坑
            if (_cfg.wifi.sta.ssid.length() && j["mode"].isNull() && _cfg.wifi.mode == WM_AP) {
                _cfg.wifi.mode = WM_STA;
                LOG_I(TAG, "检测到已填写 SSID，工作模式自动切换为 STA");
            }

            // 静态地址合法性校验
            if (!_cfg.wifi.sta.dhcp) {
                IPAddress tmp;
                if (!tmp.fromString(_cfg.wifi.sta.ip) ||
                    !tmp.fromString(_cfg.wifi.sta.gateway) ||
                    !tmp.fromString(_cfg.wifi.sta.netmask)) {
                    err = "静态 IP / 网关 / 子网掩码 格式不正确";
                    return false;
                }
            }
        }
        if (JsonObjectConst a = j["ap"]) {
            RD(a, "ssid", _cfg.wifi.ap.ssid);
            if (!a["password"].isNull()) {
                String p = a["password"].as<String>();
                if (p != "********") {
                    if (p.length() > 0 && p.length() < 8) {
                        err = "热点密码至少 8 位（留空表示开放热点）";
                        return false;
                    }
                    _cfg.wifi.ap.password = p;
                }
            }
            RD(a, "channel", _cfg.wifi.ap.channel);
            RD(a, "hidden",  _cfg.wifi.ap.hidden);
            RD(a, "maxConn", _cfg.wifi.ap.maxConn);
            RD(a, "ip",      _cfg.wifi.ap.ip);
            if (_cfg.wifi.ap.channel < 1 || _cfg.wifi.ap.channel > 13) _cfg.wifi.ap.channel = 6;
            if (_cfg.wifi.ap.maxConn < 1 || _cfg.wifi.ap.maxConn > 8)  _cfg.wifi.ap.maxConn = 4;
        }
        changed = true;
    }

    if (JsonObjectConst j = root["web"]) {
        RD(j, "username", _cfg.web.username);
        if (!j["password"].isNull()) {
            String p = j["password"].as<String>();
            if (p != "********") {
                if (p.length() < 6) { err = "网页密码至少 6 位"; return false; }
                _cfg.web.password = p;
            }
        }
        RD(j, "port",       _cfg.web.port);
        RD(j, "sessionMin", _cfg.web.sessionMin);
        RD(j, "allowLan",   _cfg.web.allowLan);
        if (_cfg.web.username.length() < 2) { err = "用户名至少 2 位"; return false; }
        if (_cfg.web.port == 0) _cfg.web.port = DEF_WEB_PORT;
        changed = true;
    }

    if (JsonObjectConst j = root["mqtt"]) {
        RD(j, "enable",          _cfg.mqtt.enable);
        RD(j, "host",            _cfg.mqtt.host);
        RD(j, "port",            _cfg.mqtt.port);
        if (!j["key"].isNull()) {
            String k = j["key"].as<String>();
            if (k != "********") _cfg.mqtt.key = k;
        }
        RD(j, "clientSuffix",    _cfg.mqtt.clientSuffix);
        RD(j, "keepalive",       _cfg.mqtt.keepalive);
        RD(j, "reportOnChange",  _cfg.mqtt.reportOnChange);
        RD(j, "reportIntervalS", _cfg.mqtt.reportIntervalS);
        RD(j, "syncOnConnect",   _cfg.mqtt.syncOnConnect);
        RD(j, "reportSuffix",    _cfg.mqtt.reportSuffix);
        if (_cfg.mqtt.reportSuffix > 2) _cfg.mqtt.reportSuffix = 0;
        if (_cfg.mqtt.keepalive < 15) _cfg.mqtt.keepalive = 15;

        if (JsonArrayConst arr = j["topics"]) {
            uint8_t n = 0;
            for (JsonObjectConst t : arr) {
                if (n >= MAX_MQTT_TOPICS) break;
                TopicConfig& tc = _cfg.mqtt.topics[n];
                tc = TopicConfig();
                RD(t, "enable", tc.enable);
                RD(t, "topic",  tc.topic);
                RD(t, "name",   tc.name);
                RD(t, "bind",   tc.bind);
                tc.topic.trim();
                if (!tc.topic.length()) continue;      // 跳过空主题
                tc.type = bafaTypeOf(tc.topic);
                if (!tc.name.length()) tc.name = tc.topic;
                n++;
            }
            for (uint8_t i = n; i < MAX_MQTT_TOPICS; i++) _cfg.mqtt.topics[i] = TopicConfig();
            _cfg.mqtt.topicCount = n;
        }
        if (_cfg.mqtt.enable && _cfg.mqtt.key.length() < 8) {
            err = "请先填写巴法云私钥（Key）";
            return false;
        }
        changed = true;
    }

    if (JsonObjectConst j = root["relay"]) {
        RD(j, "pin",           _cfg.relay.pin);
        RD(j, "activeHigh",    _cfg.relay.activeHigh);
        RD(j, "powerOn",       _cfg.relay.powerOn);
        RD(j, "minIntervalMs", _cfg.relay.minIntervalMs);
        RD(j, "autoOffS",      _cfg.relay.autoOffS);
        if (_cfg.relay.pin < 0 || _cfg.relay.pin > 39) { err = "继电器引脚号无效"; return false; }
        if (_cfg.relay.powerOn > 2) _cfg.relay.powerOn = 0;
        changed = true;
    }

    if (JsonObjectConst j = root["button"]) {
        RD(j, "enable",      _cfg.button.enable);
        RD(j, "pin",         _cfg.button.pin);
        RD(j, "activeLow",   _cfg.button.activeLow);
        RD(j, "debounceMs",  _cfg.button.debounceMs);
        RD(j, "longPressMs", _cfg.button.longPressMs);
        RD(j, "multiClick",  _cfg.button.multiClick);
        if (_cfg.button.longPressMs < 1000) _cfg.button.longPressMs = 1000;
        if (_cfg.button.debounceMs < 5)     _cfg.button.debounceMs  = 5;
        changed = true;
    }

    if (JsonObjectConst j = root["led"]) {
        RD(j, "enable",      _cfg.led.enable);
        RD(j, "pin",         _cfg.led.pin);
        RD(j, "activeHigh",  _cfg.led.activeHigh);
        RD(j, "followRelay", _cfg.led.followRelay);
        changed = true;
    }

    if (JsonObjectConst j = root["time"]) {
        RD(j, "enable",    _cfg.time.enable);
        RD(j, "ntp1",      _cfg.time.ntp1);
        RD(j, "ntp2",      _cfg.time.ntp2);
        RD(j, "ntp3",      _cfg.time.ntp3);
        RD(j, "tz",        _cfg.time.tz);
        RD(j, "intervalS", _cfg.time.intervalS);
        if (_cfg.time.intervalS < 600) _cfg.time.intervalS = 600;
        changed = true;
    }

    if (JsonArrayConst arr = root["schedules"]) {
        uint8_t i = 0;
        for (JsonObjectConst s : arr) {
            if (i >= MAX_SCHEDULES) break;
            ScheduleItem& it = _cfg.schedules[i];
            it = ScheduleItem();
            RD(s, "enable", it.enable);
            RD(s, "days",   it.days);
            RD(s, "hour",   it.hour);
            RD(s, "minute", it.minute);
            RD(s, "action", it.action);
            if (it.hour > 23 || it.minute > 59 || it.action > 2) {
                err = "定时任务参数越界";
                return false;
            }
            i++;
        }
        for (; i < MAX_SCHEDULES; i++) _cfg.schedules[i] = ScheduleItem();
        changed = true;
    }

    return changed;
}

// ---------------------------------------------------------------------------
// NVS 读写
// ---------------------------------------------------------------------------
bool ConfigManager::writeNvs(const String& json) {
    Preferences p;
    if (!p.begin(NVS_NS_CONFIG, false)) {
        LOG_E(TAG, "NVS 打开失败");
        return false;
    }
    // 先把旧值挪到备份槽，写坏了还能回滚
    String old = p.getString("cfg", "");
    if (old.length()) p.putString("cfg_bak", old);

    size_t n = p.putString("cfg", json);
    String back = p.getString("cfg", "");
    p.end();

    if (n == 0 || back != json) {
        LOG_E(TAG, "配置写入校验失败 (写入 %u / 期望 %u 字节)", (unsigned)n, (unsigned)json.length());
        return false;
    }
    return true;
}

String ConfigManager::readNvs() {
    Preferences p;
    if (!p.begin(NVS_NS_CONFIG, true)) return "";
    String s = p.getString("cfg", "");
    if (!s.length()) s = p.getString("cfg_bak", "");   // 主槽为空则尝试备份槽
    p.end();
    return s;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
bool ConfigManager::begin() {
    setDefaults();

    // 预创建运行状态命名空间：否则首次启动时以只读方式打开会打印
    // "nvs_open failed: NOT_FOUND" 的错误，属于噪音
    {
        Preferences p;
        if (p.begin(NVS_NS_STATE, false)) {
            if (!p.isKey("relay")) p.putBool("relay", false);
            p.end();
        }
    }

    String json = readNvs();
    if (!json.length()) {
        LOG_W(TAG, "未找到配置，写入出厂默认值");
        save();
        _loaded = true;
        return true;
    }

    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    if (e) {
        LOG_E(TAG, "配置解析失败(%s)，回退出厂默认值", e.c_str());
        setDefaults();
        save();
        _loaded = true;
        return false;
    }

    uint16_t fileVer = doc["version"] | 1;
    String err;
    if (!applyJson(doc.as<JsonObjectConst>(), err))
        LOG_W(TAG, "部分配置未通过校验(%s)，该部分已保持默认值", err.c_str());
    if (fileVer != CONFIG_VERSION) {
        LOG_I(TAG, "配置版本 %u -> %u，执行迁移", fileVer, CONFIG_VERSION);
        migrate(fileVer);
        _cfg.version = CONFIG_VERSION;
        save();
    }

    _loaded = true;
    LOG_I(TAG, "配置加载完成 (%u 字节, 版本 %u)", (unsigned)json.length(), _cfg.version);
    return true;
}

void ConfigManager::migrate(uint16_t fromVersion) {
    // 版本 1 -> 2：新增 LED / 定时任务分组，旧配置无此字段，已由默认值补齐
    if (fromVersion < 2) {
        if (_cfg.led.pin == 0) _cfg.led.enable = false;
    }
    // 版本 2 -> 3：继电器默认引脚由 32 调整为 16（本板实际接线）
    //  仅当旧配置从未被用户改动过时才跟随新默认值
    if (fromVersion < 3) {
        if (_cfg.relay.pin == 32) {
            LOG_W(TAG, "迁移：继电器引脚 32 -> %d（如与实际不符请在网页修改）", DEF_RELAY_PIN);
            _cfg.relay.pin = DEF_RELAY_PIN;
        }
    }
}

bool ConfigManager::save() {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    toJson(root, true);

    String json;
    serializeJson(doc, json);
    if (json.length() > 3800) {
        LOG_E(TAG, "配置过大 (%u 字节)，放弃保存", (unsigned)json.length());
        return false;
    }
    bool ok = writeNvs(json);
    if (ok) LOG_I(TAG, "配置已保存 (%u 字节)", (unsigned)json.length());
    return ok;
}

void ConfigManager::factoryReset() {
    LOG_W(TAG, "执行恢复出厂设置");
    Preferences p;
    if (p.begin(NVS_NS_CONFIG, false)) { p.clear(); p.end(); }
    if (p.begin(NVS_NS_STATE, false))  { p.clear(); p.end(); }
    setDefaults();
    save();
}

String ConfigManager::exportJson() const {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    toJson(root, true);
    root["_fw"]   = FW_VERSION;
    root["_mac"]  = WiFi.macAddress();
    String out;
    serializeJsonPretty(doc, out);
    return out;
}

bool ConfigManager::importJson(const String& text, String& err) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, text);
    if (e) { err = String("备份文件解析失败: ") + e.c_str(); return false; }
    if (!doc["version"].is<uint16_t>()) { err = "不是有效的配置备份文件"; return false; }

    AppConfig backup = _cfg;                 // 失败可回滚
    uint16_t fileVer = doc["version"] | 1;
    if (!applyJson(doc.as<JsonObjectConst>(), err)) {
        _cfg = backup;
        return false;
    }
    if (fileVer != CONFIG_VERSION) migrate(fileVer);
    _cfg.version = CONFIG_VERSION;

    if (!save()) {
        _cfg = backup;
        err = "写入 NVS 失败";
        return false;
    }
    LOG_I(TAG, "配置导入成功（原版本 %u）", fileVer);
    return true;
}
