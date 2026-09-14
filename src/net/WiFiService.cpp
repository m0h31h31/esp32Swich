#include "net/WiFiService.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "app_config.h"
#include <esp_wifi.h>
#include <algorithm>

static const char* TAG = "WIFI";

WiFiService& WiFiService::instance() {
    static WiFiService inst;
    return inst;
}

const char* WiFiService::authName(uint8_t enc) {
    switch (enc) {
        case WIFI_AUTH_OPEN:            return "开放";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2企业";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        default:                        return "未知";
    }
}

uint8_t WiFiService::signalQuality() const {
    if (!staOnline()) return 0;
    int32_t r = WiFi.RSSI();
    if (r <= -100) return 0;
    if (r >= -50)  return 100;
    return (uint8_t)(2 * (r + 100));
}

String WiFiService::ipString() const {
    if (staOnline()) return WiFi.localIP().toString();
    if (_apActive)   return WiFi.softAPIP().toString();
    return "0.0.0.0";
}

void WiFiService::setPhase(WifiPhase p) {
    if (_phase == p) return;
    _phase = p;
    if (_cb) _cb(p);
}

// ---------------------------------------------------------------------------
// 事件处理
// ---------------------------------------------------------------------------
void WiFiService::onEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOG_I(TAG, "已连接到 AP，等待分配 IP…");
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            _retryCount  = 0;
            _firstFailMs = 0;
            LOG_I(TAG, "联网成功  IP=%s  网关=%s  信号=%ddBm",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), (int)WiFi.RSSI());
            setPhase(WP_CONNECTED);
            // 联网成功后若热点仅作应急/过渡用途则关闭，但要留出宽限时间：
            // 用户很可能正连着这个热点看配网页面，立刻关掉会让他不知道配网结果
            if (_apActive && CFG.wifi.mode == WM_STA && !_apCloseAtMs) {
                _apCloseAtMs = millis() + 15000;
                LOG_I(TAG, "已联网，热点将在 15 秒后关闭（留时间给配网页面显示结果）");
            }
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            if (_phase == WP_CONNECTED) {
                _disconnectCount++;
                LOG_W(TAG, "WiFi 断开 (原因码 %u)，准备重连", reason);
            } else if (reason != _lastDisconnReason) {
                LOG_W(TAG, "连接失败 (原因码 %u)", reason);
            }
            _lastDisconnReason = reason;
            if (_staEnabled) {
                if (!_firstFailMs) _firstFailMs = millis();
                // 指数退避：15s, 30s, 60s, 120s（封顶）
                uint32_t backoff = WIFI_RECONNECT_MS * (1UL << (_retryCount > 3 ? 3 : _retryCount));
                _nextRetryMs = millis() + backoff;
                if (_retryCount < 250) _retryCount++;
                setPhase(WP_FAILED);
            }
            break;
        }

        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            LOG_I(TAG, "有设备接入热点 (当前 %d 台)", WiFi.softAPgetStationNum());
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            LOG_I(TAG, "有设备离开热点 (当前 %d 台)", WiFi.softAPgetStationNum());
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
void WiFiService::begin() {
    WiFi.persistent(false);          // 不让 SDK 自行写 NVS，统一由我们管理
    WiFi.setAutoReconnect(false);    // 重连策略自己控制，便于退避与日志
    WiFi.onEvent([this](arduino_event_id_t e, arduino_event_info_t i) { onEvent(e, i); });

    String host = ConfigManager::instance().effectiveHostname();
    WiFi.setHostname(host.c_str());
    LOG_I(TAG, "主机名: %s", host.c_str());

    applyConfig();
}

void WiFiService::applyConfig() {
    const WifiConfig& wc = CFG.wifi;
    _staEnabled = (wc.mode == WM_STA || wc.mode == WM_APSTA) &&
                  CFG.wifi.sta.ssid.length() > 0;

    wifi_mode_t target = WIFI_OFF;
    switch (wc.mode) {
        case WM_AP:    target = WIFI_AP;    break;
        case WM_STA:   target = _staEnabled ? WIFI_STA : WIFI_AP; break;  // 无凭据则退回热点
        case WM_APSTA: target = WIFI_AP_STA; break;
    }

    // 典型场景：用户正连着设备热点，在网页里填好路由器信息点保存。
    // 此时若直接切到纯 STA，热点立刻消失、手机掉线，用户既看不到结果，
    // 连错密码时也没有了补救入口。所以先用 AP+STA 过渡，
    // 等真正拿到 IP 后再（延迟）关闭热点。
    bool transitionalAp = false;
    if (target == WIFI_STA && _apActive && WiFi.softAPgetStationNum() > 0) {
        target = WIFI_AP_STA;
        transitionalAp = true;
        LOG_I(TAG, "热点上有 %d 台设备在线，先以 AP+STA 过渡",
              WiFi.softAPgetStationNum());
    }
    _apCloseAtMs = 0;
    WiFi.mode(target);

    // 发射功率：0=保持默认，其余按 dBm 设置（单位 0.25dBm，范围 2~20dBm）
    if (wc.txPower > 0) {
        int8_t dbm = wc.txPower;
        if (dbm < 2)  dbm = 2;
        if (dbm > 20) dbm = 20;
        esp_wifi_set_max_tx_power((int8_t)(dbm * 4));
        LOG_I(TAG, "WiFi 发射功率设置为 %ddBm", dbm);
    }
    applyPowerSave();

    if (target == WIFI_AP || target == WIFI_AP_STA) {
        // 过渡模式下保持热点原样运行，重启热点会把正在配网的手机踢下线
        if (transitionalAp) _apActive = true;
        else { _apActive = false; startAp(true); }
    } else {
        _apActive = false;
    }

    if (_staEnabled) {
        _retryCount  = 0;
        _firstFailMs = 0;
        connectSta();
    } else if (target == WIFI_AP) {
        setPhase(WP_AP_ONLY);
        LOG_W(TAG, "未配置 WiFi，进入热点配网模式");
    } else {
        setPhase(WP_IDLE);
    }
}

void WiFiService::applyPowerSave() {
    // 本固件不使用蓝牙，射频独占 WiFi，可以直接关掉 modem sleep 以降低响应延迟
    esp_err_t e = esp_wifi_set_ps(WIFI_PS_NONE);
    if (e != ESP_OK) LOG_W(TAG, "关闭 WiFi 省电失败 (err=%d)", (int)e);
}

void WiFiService::applyStaticIp() {
    const StaConfig& sc = CFG.wifi.sta;
    if (sc.dhcp) return;

    IPAddress ip, gw, mask, d1, d2;
    if (!ip.fromString(sc.ip) || !gw.fromString(sc.gateway) || !mask.fromString(sc.netmask)) {
        LOG_E(TAG, "静态地址配置无效，回退 DHCP");
        return;
    }
    if (!d1.fromString(sc.dns1)) d1 = gw;                      // DNS 缺省用网关
    if (!d2.fromString(sc.dns2)) d2 = IPAddress(223, 5, 5, 5); // 阿里公共 DNS

    if (WiFi.config(ip, gw, mask, d1, d2))
        LOG_I(TAG, "静态地址: %s/%s 网关 %s DNS %s",
              sc.ip.c_str(), sc.netmask.c_str(), sc.gateway.c_str(), d1.toString().c_str());
    else
        LOG_E(TAG, "静态地址设置失败");
}

void WiFiService::connectSta() {
    const StaConfig& sc = CFG.wifi.sta;
    if (!sc.ssid.length()) return;

    if (sc.dhcp) {
        // 从静态切回 DHCP 必须显式清零，否则旧地址会残留
        WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    } else {
        applyStaticIp();
    }

    LOG_I(TAG, "正在连接 \"%s\" (%s, 第 %u 次尝试)",
          sc.ssid.c_str(), sc.dhcp ? "DHCP" : "静态IP", _retryCount + 1);

    if (sc.bssidLock && sc.bssid.length() == 17) {
        uint8_t b[6];
        if (sscanf(sc.bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            WiFi.begin(sc.ssid.c_str(), sc.password.c_str(), 0, b, true);
            _connectStartMs = millis();
            setPhase(WP_CONNECTING);
            return;
        }
    }
    WiFi.begin(sc.ssid.c_str(), sc.password.c_str());
    _connectStartMs = millis();
    setPhase(WP_CONNECTING);
}

// ---------------------------------------------------------------------------
// 热点
// ---------------------------------------------------------------------------
void WiFiService::startAp(bool force) {
    if (_apActive && !force) return;

    const ApConfig& ac = CFG.wifi.ap;
    String ssid = ConfigManager::instance().effectiveApSsid();

    IPAddress apIp;
    if (!apIp.fromString(ac.ip)) apIp.fromString(DEF_AP_IP);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));

    const char* pass = ac.password.length() >= 8 ? ac.password.c_str() : nullptr;
    bool ok = WiFi.softAP(ssid.c_str(), pass, ac.channel, ac.hidden ? 1 : 0, ac.maxConn);
    _apActive = ok;

    if (ok)
        LOG_I(TAG, "热点已开启: %s (%s) IP=%s 信道%u",
              ssid.c_str(), pass ? "加密" : "开放",
              WiFi.softAPIP().toString().c_str(), ac.channel);
    else
        LOG_E(TAG, "热点开启失败");
}

void WiFiService::stopAp() {
    if (!_apActive) return;
    WiFi.softAPdisconnect(true);
    _apActive = false;
    if (_staEnabled) WiFi.mode(WIFI_STA);
}

// ---------------------------------------------------------------------------
// 扫描
// ---------------------------------------------------------------------------
bool WiFiService::startScan() {
    if (_scanning) return false;

    _scanning    = true;
    _scanReady   = false;
    _scanStartMs = millis();
    _scanChannel = 0;
    _scanFailed  = 0;
    _scanResult.clear();

    LOG_I(TAG, "开始逐信道扫描周边 WiFi（扫描期间热点可能有短暂卡顿）");
    return startChannelScan(1);
}

bool WiFiService::startChannelScan(uint8_t channel) {
    _scanChannel = channel;

    // 注意：不要在这里手动 WiFi.mode(WIFI_AP_STA)。
    // scanNetworks() 内部会调用 enableSTA(true) 自行补上 STA 接口，
    // 多余的模式切换反而会让已连接热点的客户端被踢掉。
    //
    // 参数：async=true, showHidden=true, passive=false,
    //       maxMsPerChan=200（内核据此把内部超时设为 200*20=4 秒，余量充足）,
    //       channel=指定单信道
    int16_t r = WiFi.scanNetworks(true, true, false, 200, channel);
    if (r == WIFI_SCAN_FAILED) {
        // 启动失败不中断整轮：下一次 tick() 里 scanComplete() 会立刻返回
        // WIFI_SCAN_FAILED，自然推进到下一个信道，无需在这里特殊处理
        LOG_D(TAG, "信道 %u 扫描启动失败，跳过", channel);
        return false;
    }
    return true;
}

void WiFiService::collectChannelResult() {
    int16_t n = WiFi.scanComplete();
    if (n <= 0) return;

    for (int16_t i = 0; i < n; i++) {
        ScanItem it;
        it.ssid       = WiFi.SSID(i);
        it.bssid      = WiFi.BSSIDstr(i);
        it.rssi       = WiFi.RSSI(i);
        it.channel    = WiFi.channel(i);
        it.encryption = (uint8_t)WiFi.encryptionType(i);
        if (!it.ssid.length()) it.ssid = "(隐藏网络)";

        // 同名 SSID（多 AP / Mesh）只保留信号最强的那个
        bool merged = false;
        for (auto& e : _scanResult) {
            if (e.ssid == it.ssid) {
                if (it.rssi > e.rssi) e = it;
                merged = true;
                break;
            }
        }
        if (!merged && _scanResult.size() < 40) _scanResult.push_back(it);
    }
}

void WiFiService::finishScan() {
    WiFi.scanDelete();
    std::sort(_scanResult.begin(), _scanResult.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.rssi > b.rssi; });
    _scanning  = false;
    _scanReady = true;
    LOG_I(TAG, "扫描完成，发现 %u 个网络，耗时 %lu ms%s",
          (unsigned)_scanResult.size(), (unsigned long)(millis() - _scanStartMs),
          _scanFailed ? "（部分信道未响应）" : "");
}

uint8_t WiFiService::scanProgress() const {
    if (!_scanning) return _scanReady ? 100 : 0;
    return (uint8_t)(_scanChannel * 100 / 13);
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void WiFiService::tick() {
    uint32_t now = millis();

    // ---- 逐信道扫描推进 ----
    if (_scanning) {
        int16_t n = WiFi.scanComplete();

        // n >= 0 本信道完成；WIFI_SCAN_FAILED 表示该信道失败或内核内部超时。
        // 两种情况都继续扫下一个信道，单个信道的问题不该让整轮扫描作废。
        if (n >= 0 || n == WIFI_SCAN_FAILED) {
            if (n >= 0) collectChannelResult();
            else        _scanFailed++;

            if (_scanChannel >= 13) {
                finishScan();
            } else {
                WiFi.scanDelete();
                startChannelScan(_scanChannel + 1);
            }
        } else if (now - _scanStartMs > 30000) {
            // 整轮兜底超时：正常情况下 13 个信道约 3 秒内跑完
            LOG_W(TAG, "扫描整体超时，返回已扫到的 %u 个网络",
                  (unsigned)_scanResult.size());
            finishScan();
        }
    }

    // ---- 联网成功后延迟关闭过渡热点 ----
    if (_apCloseAtMs && (int32_t)(now - _apCloseAtMs) >= 0) {
        _apCloseAtMs = 0;
        if (staOnline() && CFG.wifi.mode == WM_STA) {
            LOG_I(TAG, "配网完成，关闭热点。后续请通过 http://%s 或 http://%s.local 访问",
                  WiFi.localIP().toString().c_str(),
                  ConfigManager::instance().effectiveHostname().c_str());
            stopAp();
        }
    }

    if (!_staEnabled) return;

    // ---- 连接超时判定 ----
    if (_phase == WP_CONNECTING && now - _connectStartMs > WIFI_CONNECT_TIMEOUT) {
        LOG_W(TAG, "连接超时 (%us)", WIFI_CONNECT_TIMEOUT / 1000);
        WiFi.disconnect();
        if (!_firstFailMs) _firstFailMs = millis();
        uint32_t backoff = WIFI_RECONNECT_MS * (1UL << (_retryCount > 3 ? 3 : _retryCount));
        _nextRetryMs = now + backoff;
        if (_retryCount < 250) _retryCount++;
        setPhase(WP_FAILED);
    }

    // ---- 退避后重连 ----
    if (_phase == WP_FAILED && (int32_t)(now - _nextRetryMs) >= 0) {
        connectSta();
    }

    // ---- 长时间连不上则开应急热点 ----
    if (CFG.wifi.apFallback && !_apActive && _firstFailMs &&
        _phase != WP_CONNECTED &&
        (now - _firstFailMs) > (uint32_t)CFG.wifi.apFallbackS * 1000UL) {
        LOG_W(TAG, "连接 %us 未成功，开启应急热点以便现场补救", CFG.wifi.apFallbackS);
        if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
        startAp(true);
    }
}

bool WiFiService::tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs) {
    LOG_I(TAG, "测试连接 \"%s\"…", ssid.c_str());
    wifi_mode_t old = WiFi.getMode();
    if (old == WIFI_AP) WiFi.mode(WIFI_AP_STA);

    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (WiFi.status() == WL_CONNECTED) {
            LOG_I(TAG, "测试连接成功，IP=%s", WiFi.localIP().toString().c_str());
            return true;
        }
        delay(120);
    }
    LOG_W(TAG, "测试连接失败");
    WiFi.disconnect();
    return false;
}

void WiFiService::toJson(JsonObject root) {
    const char* phaseText[] = {"未启动", "连接中", "已连接", "连接失败", "热点模式"};
    root["phase"]     = _phase;
    root["phaseText"] = phaseText[_phase <= WP_AP_ONLY ? _phase : 0];
    root["mode"]      = CFG.wifi.mode;

    JsonObject sta = root["sta"].to<JsonObject>();
    sta["online"]  = staOnline();
    sta["ssid"]    = CFG.wifi.sta.ssid;
    sta["ip"]      = staOnline() ? WiFi.localIP().toString()   : String("-");
    sta["gateway"] = staOnline() ? WiFi.gatewayIP().toString() : String("-");
    sta["netmask"] = staOnline() ? WiFi.subnetMask().toString(): String("-");
    sta["dns"]     = staOnline() ? WiFi.dnsIP().toString()     : String("-");
    sta["mac"]     = WiFi.macAddress();
    sta["rssi"]    = rssi();
    sta["quality"] = signalQuality();
    sta["channel"] = staOnline() ? WiFi.channel() : 0;
    sta["bssid"]   = staOnline() ? WiFi.BSSIDstr() : String("-");
    sta["dhcp"]    = CFG.wifi.sta.dhcp;
    sta["retries"] = _retryCount;
    sta["disconnects"] = _disconnectCount;

    JsonObject ap = root["ap"].to<JsonObject>();
    ap["active"]  = _apActive;
    ap["ssid"]    = ConfigManager::instance().effectiveApSsid();
    ap["ip"]      = _apActive ? WiFi.softAPIP().toString() : String("-");
    ap["clients"] = _apActive ? WiFi.softAPgetStationNum() : 0;
    ap["mac"]     = WiFi.softAPmacAddress();

    root["hostname"] = ConfigManager::instance().effectiveHostname();
}
