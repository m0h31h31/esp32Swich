#include "net/WebService.h"
#include "net/WiFiService.h"
#include "net/MqttService.h"
#include "net/TimeService.h"
#include "net/OtaService.h"
#include "io/RelayController.h"
#include "io/ButtonHandler.h"
#include "io/StatusLed.h"
#include "app/ScheduleService.h"
#include "core/ConfigManager.h"
#include "core/SysInfo.h"
#include "core/Logger.h"
#include "web_assets.h"

#include <esp_task_wdt.h>

static const char* TAG = "WEB";
static const byte  DNS_PORT = 53;

WebService& WebService::instance() {
    static WebService inst;
    return inst;
}

// ---------------------------------------------------------------------------
// 启动 / 停止
// ---------------------------------------------------------------------------
void WebService::begin() {
    _port = CFG.web.port ? CFG.web.port : DEF_WEB_PORT;
    _server = new WebServer(_port);

    // 需要读取 Cookie 做会话校验
    const char* headers[] = {"Cookie", "X-Auth-Token", "Content-Length"};
    _server->collectHeaders(headers, 3);

    registerRoutes();
    _server->begin();
    _running = true;

    LOG_I(TAG, "网页服务已启动，端口 %u（默认账号 %s）",
          _port, CFG.web.username.c_str());
    if (ConfigManager::instance().isDefaultWebPassword())
        LOG_W(TAG, "当前仍在使用默认密码，请尽快在【系统设置】中修改！");
}

void WebService::restart() {
    if (_server) {
        _server->close();
        delete _server;
        _server = nullptr;
    }
    _running = false;
    begin();
}

void WebService::updateCaptivePortal(bool apActive) {
    if (apActive && !_portal) {
        if (!_dns) _dns = new DNSServer();
        _dns->setErrorReplyCode(DNSReplyCode::NoError);
        // 把所有域名解析到设备自身，连上热点即自动跳出配置页
        if (_dns->start(DNS_PORT, "*", WiFi.softAPIP())) {
            _portal = true;
            LOG_I(TAG, "强制门户已开启 (http://%s)", WiFi.softAPIP().toString().c_str());
        }
    } else if (!apActive && _portal) {
        if (_dns) _dns->stop();
        _portal = false;
        LOG_I(TAG, "强制门户已关闭");
    }
}

void WebService::tick() {
    if (_portal && _dns) _dns->processNextRequest();
    if (_running && _server) _server->handleClient();

    // 延后执行的危险操作：先把 HTTP 响应发完再动手
    if (_restartAfterReply) { _restartAfterReply = false; delay(200); restart(); }
    if (_factoryAfterReply) {
        _factoryAfterReply = false;
        delay(300);
        ConfigManager::instance().factoryReset();
        OtaService::instance().scheduleReboot(500);
    }
    if (_rebootAfterReply) {
        _rebootAfterReply = false;
        OtaService::instance().scheduleReboot(800);
    }

    // 清理过期会话
    static uint32_t lastClean = 0;
    if (millis() - lastClean > 60000) {
        lastClean = millis();
        for (auto& s : _sessions)
            if (s.token[0] && (int32_t)(millis() - s.expireMs) >= 0) {
                LOG_D(TAG, "会话过期: %s", s.clientIp.c_str());
                memset(s.token, 0, sizeof(s.token));
            }
    }
}

// ---------------------------------------------------------------------------
// 通用回复
// ---------------------------------------------------------------------------
void WebService::sendJson(int code, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(code, "application/json; charset=utf-8", out);
}

void WebService::sendOk(const String& msg) {
    JsonDocument d;
    d["ok"]  = true;
    d["msg"] = msg;
    sendJson(200, d);
}

void WebService::sendErr(int code, const String& msg) {
    JsonDocument d;
    d["ok"]  = false;
    d["msg"] = msg;
    sendJson(code, d);
}

bool WebService::readBody(JsonDocument& doc) {
    if (!_server->hasArg("plain")) { sendErr(400, "请求体为空"); return false; }
    DeserializationError e = deserializeJson(doc, _server->arg("plain"));
    if (e) { sendErr(400, String("JSON 解析失败: ") + e.c_str()); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 会话管理
// ---------------------------------------------------------------------------
String WebService::createSession() {
    char token[33];
    // 使用硬件随机数发生器生成令牌
    for (uint8_t i = 0; i < 32; i += 8) {
        uint32_t r = esp_random();
        snprintf(token + i, 9, "%08x", r);
    }
    token[32] = '\0';

    // 找空位；没有空位则淘汰最久未使用的
    WebSession* slot = nullptr;
    for (auto& s : _sessions)
        if (!s.token[0]) { slot = &s; break; }
    if (!slot) {
        slot = &_sessions[0];
        for (auto& s : _sessions)
            if (s.lastSeen < slot->lastSeen) slot = &s;
        LOG_W(TAG, "会话已满，淘汰最旧的一个");
    }

    strncpy(slot->token, token, sizeof(slot->token));
    slot->expireMs = millis() + (uint32_t)CFG.web.sessionMin * 60000UL;
    slot->lastSeen = millis();
    slot->clientIp = _server->client().remoteIP().toString();
    _loginCount++;
    return String(token);
}

void WebService::destroySession(const String& token) {
    for (auto& s : _sessions)
        if (token.equals(s.token)) memset(s.token, 0, sizeof(s.token));
}

String WebService::tokenFromRequest() {
    // 优先取 Header，其次解析 Cookie
    if (_server->hasHeader("X-Auth-Token")) {
        String t = _server->header("X-Auth-Token");
        t.trim();
        if (t.length()) return t;
    }
    if (_server->hasHeader("Cookie")) {
        String c = _server->header("Cookie");
        int p = c.indexOf("ESPTOKEN=");
        if (p >= 0) {
            int end = c.indexOf(';', p);
            String t = (end < 0) ? c.substring(p + 9) : c.substring(p + 9, end);
            t.trim();
            return t;
        }
    }
    return "";
}

bool WebService::checkAuth() {
    String token = tokenFromRequest();
    if (token.length() == 32) {
        for (auto& s : _sessions) {
            if (!s.token[0] || !token.equals(s.token)) continue;
            if ((int32_t)(millis() - s.expireMs) >= 0) {
                memset(s.token, 0, sizeof(s.token));
                break;
            }
            // 活跃会话自动续期
            s.lastSeen = millis();
            s.expireMs = millis() + (uint32_t)CFG.web.sessionMin * 60000UL;
            return true;
        }
    }
    sendErr(401, "未登录或会话已过期");
    return false;
}

bool WebService::isLocked() const {
    return _lockUntilMs && (int32_t)(millis() - _lockUntilMs) < 0;
}

// ---------------------------------------------------------------------------
// 路由注册
// ---------------------------------------------------------------------------
void WebService::registerRoutes() {
    WebServer* s = _server;

    // 统计请求数
    s->on("/", HTTP_GET, [this]() { _reqCount++; handleRoot(); });

    // ---- 认证 ----
    s->on("/api/login",  HTTP_POST, [this]() { _reqCount++; handleLogin(); });
    s->on("/api/logout", HTTP_POST, [this]() { _reqCount++; handleLogout(); });

    // ---- 只读信息 ----
    s->on("/api/status",  HTTP_GET, [this]() { _reqCount++; handleStatus(); });
    s->on("/api/sysinfo", HTTP_GET, [this]() { _reqCount++; handleSysInfo(); });
    s->on("/api/logs",    HTTP_GET, [this]() { _reqCount++; handleLogs(); });
    s->on("/api/logs/clear", HTTP_POST, [this]() { _reqCount++; handleLogsClear(); });

    // ---- 配置 ----
    s->on("/api/config", HTTP_GET,  [this]() { _reqCount++; handleConfigGet(); });
    s->on("/api/config", HTTP_POST, [this]() { _reqCount++; handleConfigPost(); });
    s->on("/api/config/export", HTTP_GET,  [this]() { _reqCount++; handleConfigExport(); });
    s->on("/api/config/import", HTTP_POST, [this]() { _reqCount++; handleConfigImport(); });

    // ---- 控制 ----
    s->on("/api/relay",       HTTP_POST, [this]() { _reqCount++; handleRelay(); });
    s->on("/api/relay/probe", HTTP_POST, [this]() { _reqCount++; handleRelayProbe(); });

    // ---- 网络 ----
    s->on("/api/wifi/scan", HTTP_GET,  [this]() { _reqCount++; handleWifiScan(); });
    s->on("/api/wifi/test", HTTP_POST, [this]() { _reqCount++; handleWifiTest(); });

    // ---- 巴法云 ----
    s->on("/api/mqtt/reconnect", HTTP_POST, [this]() { _reqCount++; handleMqttReconnect(); });
    s->on("/api/mqtt/publish",   HTTP_POST, [this]() { _reqCount++; handleMqttPublish(); });

    // ---- 时间 / 定时 ----
    s->on("/api/time/sync", HTTP_POST, [this]() { _reqCount++; handleTimeSync(); });
    s->on("/api/schedules", HTTP_GET,  [this]() { _reqCount++; handleSchedules(); });

    // ---- OTA ----
    s->on("/api/ota/status", HTTP_GET,  [this]() { _reqCount++; handleOtaStatus(); });
    s->on("/api/ota/url",    HTTP_POST, [this]() { _reqCount++; handleOtaUrl(); });
    s->on("/api/ota/check",  HTTP_POST, [this]() { _reqCount++; handleOtaCheck(); });
    s->on("/api/ota/upload", HTTP_POST,
          [this]() { _reqCount++; handleOtaUploadDone(); },
          [this]() { handleOtaUploadData(); });

    // ---- 系统 ----
    s->on("/api/system/reboot",  HTTP_POST, [this]() { _reqCount++; handleReboot(); });
    s->on("/api/system/factory", HTTP_POST, [this]() { _reqCount++; handleFactoryReset(); });

    s->onNotFound([this]() { _reqCount++; handleNotFound(); });
}

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------
void WebService::handleRoot() {
    _server->sendHeader("Content-Encoding", "gzip");
    _server->sendHeader("Cache-Control", "public, max-age=600");
    _server->send_P(200, "text/html; charset=utf-8",
                    (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void WebService::handleNotFound() {
    // 强制门户：AP 模式下把任意请求都导回配置首页
    if (_portal) {
        String url = "http://" + WiFi.softAPIP().toString() + "/";
        _server->sendHeader("Location", url, true);
        _server->send(302, "text/plain", "");
        return;
    }
    if (_server->uri().startsWith("/api/")) {
        sendErr(404, "接口不存在: " + _server->uri());
        return;
    }
    // 单页应用的其他路径一律返回首页
    handleRoot();
}

// ---------------------------------------------------------------------------
// 登录
// ---------------------------------------------------------------------------
void WebService::handleLogin() {
    if (isLocked()) {
        uint32_t left = (_lockUntilMs - millis()) / 1000;
        sendErr(429, "密码错误次数过多，请 " + String(left) + " 秒后再试");
        return;
    }

    JsonDocument doc;
    if (!readBody(doc)) return;

    String user = doc["username"] | "";
    String pass = doc["password"] | "";
    String ip   = _server->client().remoteIP().toString();

    if (user == CFG.web.username && pass == CFG.web.password) {
        _failCount   = 0;
        _lockUntilMs = 0;
        String token = createSession();

        JsonDocument d;
        d["ok"]        = true;
        d["msg"]       = "登录成功";
        d["token"]     = token;
        d["expireMin"] = CFG.web.sessionMin;
        d["defaultPwd"]= ConfigManager::instance().isDefaultWebPassword();
        d["device"]    = CFG.sys.deviceName;

        _server->sendHeader("Set-Cookie",
            "ESPTOKEN=" + token + "; Path=/; Max-Age=" +
            String((uint32_t)CFG.web.sessionMin * 60) + "; SameSite=Lax");
        sendJson(200, d);
        LOG_I(TAG, "用户 %s 登录成功 (来自 %s)", user.c_str(), ip.c_str());
        return;
    }

    _failCount++;
    LOG_W(TAG, "登录失败 (%u/%u)，来自 %s", _failCount, LOGIN_FAIL_LIMIT, ip.c_str());
    if (_failCount >= LOGIN_FAIL_LIMIT) {
        _lockUntilMs = millis() + LOGIN_LOCK_MS;
        _failCount   = 0;
        LOG_E(TAG, "连续登录失败，已锁定 %u 分钟", LOGIN_LOCK_MS / 60000);
        sendErr(429, "密码错误次数过多，已锁定 " + String(LOGIN_LOCK_MS / 60000) + " 分钟");
        return;
    }
    // 固定延时，削弱在线爆破的效率
    delay(500);
    sendErr(401, "用户名或密码错误（剩余 " +
                 String(LOGIN_FAIL_LIMIT - _failCount) + " 次机会）");
}

void WebService::handleLogout() {
    destroySession(tokenFromRequest());
    _server->sendHeader("Set-Cookie", "ESPTOKEN=; Path=/; Max-Age=0");
    sendOk("已退出登录");
}

// ---------------------------------------------------------------------------
// 状态 / 信息
// ---------------------------------------------------------------------------
void WebService::handleStatus() {
    if (!checkAuth()) return;

    JsonDocument doc;
    doc["ok"] = true;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["name"]     = CFG.sys.deviceName;
    dev["hostname"] = ConfigManager::instance().effectiveHostname();
    dev["fw"]       = FW_VERSION;
    dev["defaultPwd"] = ConfigManager::instance().isDefaultWebPassword();

    JsonObject rt = doc["runtime"].to<JsonObject>();
    SysInfo::toJsonRuntime(rt);

    JsonObject relay = doc["relay"].to<JsonObject>();
    RelayController::instance().toJson(relay);

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    WiFiService::instance().toJson(wifi);

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    MqttService::instance().toJson(mqtt);

    JsonArray topics = doc["topics"].to<JsonArray>();
    MqttService::instance().topicsToJson(topics);

    JsonObject tm = doc["time"].to<JsonObject>();
    TimeService::instance().toJson(tm);

    JsonObject ota = doc["ota"].to<JsonObject>();
    OtaService::instance().toJson(ota);

    String nextDesc;
    int32_t next = ScheduleService::instance().secondsToNext(nextDesc);
    JsonObject sch = doc["schedule"].to<JsonObject>();
    sch["nextSec"]  = next;
    sch["nextDesc"] = nextDesc;

    JsonObject web = doc["web"].to<JsonObject>();
    toJson(web);

    doc["logLastId"] = Logger::lastId();
    sendJson(200, doc);
}

void WebService::handleSysInfo() {
    if (!checkAuth()) return;
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject root = doc["info"].to<JsonObject>();
    SysInfo::toJsonStatic(root);
    sendJson(200, doc);
}

void WebService::handleLogs() {
    if (!checkAuth()) return;
    uint32_t since = _server->hasArg("since") ? _server->arg("since").toInt() : 0;

    String items;
    size_t n = Logger::toJson(since, items, 60);

    String out = "{\"ok\":true,\"lastId\":";
    out += Logger::lastId();
    out += ",\"count\":";
    out += n;
    out += ",\"items\":";
    out += items;
    out += "}";
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json; charset=utf-8", out);
}

void WebService::handleLogsClear() {
    if (!checkAuth()) return;
    Logger::clear();
    LOG_I(TAG, "日志已被清空");
    sendOk("日志已清空");
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
void WebService::handleConfigGet() {
    if (!checkAuth()) return;
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject cfg = doc["config"].to<JsonObject>();
    ConfigManager::instance().toJson(cfg, false);   // 不返回明文密码
    sendJson(200, doc);
}

void WebService::handleConfigPost() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    ConfigManager& cm = ConfigManager::instance();
    // 记录变更前的关键参数，决定保存后需要重启哪些服务
    uint16_t oldPort   = CFG.web.port;
    int8_t   oldRPin   = CFG.relay.pin;
    bool     oldRLevel = CFG.relay.activeHigh;
    uint8_t  oldWMode  = CFG.wifi.mode;
    String   oldSsid   = CFG.wifi.sta.ssid;
    bool     oldDhcp   = CFG.wifi.sta.dhcp;
    String   oldIp     = CFG.wifi.sta.ip;

    String err;
    if (!cm.applyJson(doc.as<JsonObjectConst>(), err)) {
        sendErr(400, err.length() ? err : "配置校验失败");
        return;
    }
    if (!cm.save()) { sendErr(500, "配置写入 NVS 失败"); return; }

    // ---- 按需热应用，尽量不重启 ----
    Logger::setLevel((LogLevel)CFG.sys.logLevel);

    if (!doc["relay"].isNull() &&
        (oldRPin != CFG.relay.pin || oldRLevel != CFG.relay.activeHigh))
        RelayController::instance().applyConfig();

    if (!doc["button"].isNull()) ButtonHandler::instance().applyConfig();
    if (!doc["led"].isNull())    StatusLed::instance().applyConfig();
    if (!doc["mqtt"].isNull())   MqttService::instance().applyConfig();
    if (!doc["time"].isNull())   TimeService::instance().applyConfig();

    bool wifiChanged = !doc["wifi"].isNull() &&
                       (oldWMode != CFG.wifi.mode || oldSsid != CFG.wifi.sta.ssid ||
                        oldDhcp != CFG.wifi.sta.dhcp || oldIp != CFG.wifi.sta.ip);
    bool portChanged = (oldPort != CFG.web.port);

    JsonDocument d;
    d["ok"]  = true;
    d["msg"] = "配置已保存";
    d["wifiRestart"] = wifiChanged;
    d["portChanged"] = portChanged;
    if (portChanged) d["newPort"] = CFG.web.port;
    sendJson(200, d);

    LOG_I(TAG, "配置已通过网页更新");
    if (wifiChanged) {
        LOG_W(TAG, "网络参数变更，正在重新连接…");
        _server->client().stop();
        delay(150);
        WiFiService::instance().applyConfig();
    }
    if (portChanged) _restartAfterReply = true;
}

void WebService::handleConfigExport() {
    if (!checkAuth()) return;
    String json = ConfigManager::instance().exportJson();
    String fname = "esp32sw-" + ConfigManager::macSuffix() + "-backup.json";
    _server->sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    _server->send(200, "application/json; charset=utf-8", json);
    LOG_I(TAG, "配置已导出 (%u 字节)", (unsigned)json.length());
}

void WebService::handleConfigImport() {
    if (!checkAuth()) return;
    if (!_server->hasArg("plain")) { sendErr(400, "请求体为空"); return; }

    String err;
    if (!ConfigManager::instance().importJson(_server->arg("plain"), err)) {
        sendErr(400, err);
        return;
    }
    sendOk("配置导入成功，设备将在 2 秒后重启以应用全部设置");
    _rebootAfterReply = true;
}

// ---------------------------------------------------------------------------
// 继电器
// ---------------------------------------------------------------------------
void WebService::handleRelay() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    RelayController& r = RelayController::instance();
    String action = doc["action"] | "toggle";

    bool ok;
    if      (action == "on")     ok = r.set(true,  RS_WEB);
    else if (action == "off")    ok = r.set(false, RS_WEB);
    else if (action == "toggle") ok = r.toggle(RS_WEB);
    else { sendErr(400, "action 只能是 on / off / toggle"); return; }

    JsonDocument d;
    d["ok"]    = ok;
    d["state"] = r.state();
    d["msg"]   = ok ? (r.state() ? "已开启" : "已关闭") : "操作过于频繁，请稍候再试";
    sendJson(ok ? 200 : 429, d);
}

void WebService::handleRelayProbe() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    int      pin     = doc["pin"]     | -1;
    uint16_t pulse   = doc["pulseMs"] | 800;
    bool     high    = doc["activeHigh"] | CFG.relay.activeHigh;
    if (pulse > 1500) pulse = 1500;
    if (pin < 0) { sendErr(400, "缺少 pin 参数"); return; }

    // 探测期间会阻塞最多 1.5 秒，先喂一次看门狗
    esp_task_wdt_reset();
    bool ok = RelayController::probePin(pin, pulse, high);
    esp_task_wdt_reset();

    if (!ok) { sendErr(400, "IO" + String(pin) + " 为保留引脚或非法引脚，无法探测"); return; }
    sendOk("已向 IO" + String(pin) + " 输出测试脉冲，若听到继电器动作说明该引脚正确");
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void WebService::handleWifiScan() {
    if (!checkAuth()) return;
    WiFiService& w = WiFiService::instance();

    // start=1 触发新扫描；否则读取上次结果
    if (_server->hasArg("start") || (!w.scanning() && !w.scanReady())) {
        w.startScan();
        JsonDocument d;
        d["ok"]       = true;
        d["scanning"] = true;
        d["progress"] = w.scanProgress();
        d["msg"]      = "正在扫描，请稍候…";
        sendJson(200, d);
        return;
    }

    if (w.scanning()) {
        JsonDocument d;
        d["ok"]       = true;
        d["scanning"] = true;
        d["progress"] = w.scanProgress();
        d["msg"]      = "扫描进行中";
        sendJson(200, d);
        return;
    }

    JsonDocument doc;
    doc["ok"]       = true;
    doc["scanning"] = false;
    doc["progress"] = 100;
    JsonArray arr = doc["list"].to<JsonArray>();
    for (const auto& it : w.scanResult()) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]    = it.ssid;
        o["bssid"]   = it.bssid;
        o["rssi"]    = it.rssi;
        o["channel"] = it.channel;
        o["enc"]     = it.encryption;
        o["encName"] = WiFiService::authName(it.encryption);
        o["quality"] = it.rssi <= -100 ? 0 : (it.rssi >= -50 ? 100 : 2 * (it.rssi + 100));
    }
    sendJson(200, doc);
}

void WebService::handleWifiTest() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    String ssid = doc["ssid"] | "";
    String pass = doc["password"] | "";
    if (!ssid.length()) { sendErr(400, "请填写 SSID"); return; }
    // 掩码表示沿用已保存的密码
    if (pass == "********") pass = CFG.wifi.sta.password;

    sendOk("正在后台测试连接，请在【运行日志】中查看结果");
    // 回复已发出，再执行会阻塞的连接测试
    _server->client().stop();
    esp_task_wdt_reset();
    bool ok = WiFiService::instance().tryConnect(ssid, pass, 15000);
    esp_task_wdt_reset();
    LOG_I(TAG, "连接测试结果: %s", ok ? "成功" : "失败");
    if (!ok) WiFiService::instance().applyConfig();   // 失败则恢复原配置
}

// ---------------------------------------------------------------------------
// 巴法云
// ---------------------------------------------------------------------------
void WebService::handleMqttReconnect() {
    if (!checkAuth()) return;
    MqttService::instance().reconnectNow();
    sendOk("已触发重新连接");
}

void WebService::handleMqttPublish() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    String topic   = doc["topic"] | "";
    String payload = doc["payload"] | "";
    if (!topic.length()) { sendErr(400, "缺少主题"); return; }
    if (!MqttService::instance().connected()) { sendErr(503, "巴法云未连接"); return; }

    bool ok = MqttService::instance().publish(topic, payload);
    if (ok) sendOk("消息已发送");
    else    sendErr(500, "发送失败");
}

// ---------------------------------------------------------------------------
// 时间 / 定时
// ---------------------------------------------------------------------------
void WebService::handleTimeSync() {
    if (!checkAuth()) return;
    if (!WiFiService::instance().staOnline()) { sendErr(503, "设备未联网，无法校时"); return; }
    TimeService::instance().requestSync();
    sendOk("已触发时间同步");
}

void WebService::handleSchedules() {
    if (!checkAuth()) return;
    JsonDocument doc;
    doc["ok"] = true;
    JsonArray arr = doc["schedules"].to<JsonArray>();
    ScheduleService::instance().toJson(arr);
    String desc;
    doc["nextSec"]  = ScheduleService::instance().secondsToNext(desc);
    doc["nextDesc"] = desc;
    sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
void WebService::handleOtaStatus() {
    if (!checkAuth()) return;
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject o = doc["ota"].to<JsonObject>();
    OtaService::instance().toJson(o);
    sendJson(200, doc);
}

void WebService::handleOtaUrl() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    String url = doc["url"] | "";
    if (!url.length()) url = CFG.sys.otaUrl;
    if (!url.length()) { sendErr(400, "请填写固件地址"); return; }

    if (!OtaService::instance().startUrlUpdate(url)) {
        sendErr(400, OtaService::instance().message());
        return;
    }
    sendOk("已开始远程升级，请勿断电，可在日志中查看进度");
}

void WebService::handleOtaCheck() {
    if (!checkAuth()) return;
    String json, err;
    if (!OtaService::instance().checkUpdate(json, err)) { sendErr(400, err); return; }

    String out = "{\"ok\":true,\"current\":\"" FW_VERSION "\",\"remote\":";
    out += json;
    out += "}";
    _server->send(200, "application/json; charset=utf-8", out);
}

void WebService::handleOtaUploadData() {
    HTTPUpload& up = _server->upload();
    OtaService& ota = OtaService::instance();

    if (up.status == UPLOAD_FILE_START) {
        // 上传开始时校验登录状态，未登录直接拒绝
        String token = tokenFromRequest();
        _otaAuthOk = false;
        if (token.length() == 32)
            for (auto& s : _sessions)
                if (s.token[0] && token.equals(s.token) &&
                    (int32_t)(millis() - s.expireMs) < 0) { _otaAuthOk = true; break; }

        if (!_otaAuthOk) { LOG_W(TAG, "未授权的固件上传请求已被拒绝"); return; }

        // Content-Length 含 multipart 表单开销，不能直接当固件大小用；
        // 传 0 让 Update 以 UPDATE_SIZE_UNKNOWN 方式擦写，前端自行显示上传进度
        ota.uploadBegin(up.filename, 0);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!_otaAuthOk) return;
        esp_task_wdt_reset();
        ota.uploadWrite(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (!_otaAuthOk) return;
        ota.uploadEnd(false);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (!_otaAuthOk) return;
        ota.uploadEnd(true);
    }
}

void WebService::handleOtaUploadDone() {
    if (!_otaAuthOk) { sendErr(401, "未登录或会话已过期"); return; }

    OtaService& ota = OtaService::instance();
    JsonDocument d;
    d["ok"]  = (ota.phase() == OTA_SUCCESS);
    d["msg"] = ota.message();
    JsonObject o = d["ota"].to<JsonObject>();
    ota.toJson(o);
    _server->sendHeader("Connection", "close");
    sendJson(ota.phase() == OTA_SUCCESS ? 200 : 400, d);
}

// ---------------------------------------------------------------------------
// 系统
// ---------------------------------------------------------------------------
void WebService::handleReboot() {
    if (!checkAuth()) return;
    sendOk("设备将在 1 秒后重启");
    LOG_W(TAG, "收到网页重启指令");
    _rebootAfterReply = true;
}

void WebService::handleFactoryReset() {
    if (!checkAuth()) return;
    JsonDocument doc;
    if (!readBody(doc)) return;

    String confirm = doc["confirm"] | "";
    if (confirm != "RESET") {
        sendErr(400, "请在确认框中输入 RESET 以确认恢复出厂设置");
        return;
    }
    sendOk("正在恢复出厂设置，设备将重启");
    LOG_W(TAG, "收到网页恢复出厂设置指令");
    _factoryAfterReply = true;
}

// ---------------------------------------------------------------------------
// 自身状态
// ---------------------------------------------------------------------------
void WebService::toJson(JsonObject root) {
    root["port"]       = _port;
    root["running"]    = _running;
    root["portal"]     = _portal;
    root["requests"]   = _reqCount;
    root["logins"]     = _loginCount;
    root["locked"]     = isLocked();
    root["lockLeft"]   = isLocked() ? (_lockUntilMs - millis()) / 1000 : 0;
    uint8_t active = 0;
    for (auto& s : _sessions) if (s.token[0]) active++;
    root["sessions"]   = active;
}
