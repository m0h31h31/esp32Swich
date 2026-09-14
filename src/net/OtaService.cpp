#include "net/OtaService.h"
#include "net/WiFiService.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "app_config.h"

#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>

static const char* TAG = "OTA";

OtaService& OtaService::instance() {
    static OtaService inst;
    return inst;
}

void OtaService::setMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_message, sizeof(_message), fmt, ap);
    va_end(ap);
}

void OtaService::begin() {
    _phase    = OTA_IDLE;
    _progress = 0;
    setMessage("就绪");

    // 若上一次 OTA 后首次启动，此处可做自检；自检通过才标记分区有效
    LOG_I(TAG, "OTA 就绪，可用空间 %u 字节", (unsigned)ESP.getFreeSketchSpace());
}

void OtaService::scheduleReboot(uint32_t delayMs) {
    _rebootAtMs = millis() + delayMs;
}

// ---------------------------------------------------------------------------
// 网页上传升级
// ---------------------------------------------------------------------------
bool OtaService::uploadBegin(const String& filename, size_t totalSize) {
    if (_phase == OTA_RUNNING) {
        setMessage("已有升级任务正在进行");
        return false;
    }
    size_t maxSize = ESP.getFreeSketchSpace();
    if (totalSize && totalSize > maxSize) {
        _phase = OTA_FAILED;
        setMessage("固件过大: %u 字节 > 可用 %u 字节", (unsigned)totalSize, (unsigned)maxSize);
        LOG_E(TAG, "%s", _message);
        return false;
    }

    if (_beforeCb) _beforeCb();     // 让 MQTT 先释放资源

    _phase          = OTA_RUNNING;
    _progress       = 0;
    _written        = 0;
    _total          = totalSize;
    _headerChecked  = false;

    if (!Update.begin(totalSize ? totalSize : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        _phase = OTA_FAILED;
        setMessage("初始化失败: %s", Update.errorString());
        LOG_E(TAG, "%s", _message);
        return false;
    }
    LOG_W(TAG, "开始升级: %s (%u 字节)", filename.c_str(), (unsigned)totalSize);
    setMessage("正在写入固件…");
    return true;
}

bool OtaService::uploadWrite(const uint8_t* data, size_t len) {
    if (_phase != OTA_RUNNING) return false;

    // 校验 ESP32 固件头魔数，防止误传了别的文件
    if (!_headerChecked && len > 0) {
        _headerChecked = true;
        if (data[0] != 0xE9) {
            Update.abort();
            _phase = OTA_FAILED;
            setMessage("不是有效的 ESP32 固件 (文件头 0x%02X)", data[0]);
            LOG_E(TAG, "%s", _message);
            return false;
        }
    }

    if (Update.write((uint8_t*)data, len) != len) {
        Update.abort();
        _phase = OTA_FAILED;
        setMessage("写入失败: %s", Update.errorString());
        LOG_E(TAG, "%s", _message);
        return false;
    }

    _written += len;
    if (_total) {
        uint8_t p = (uint8_t)(_written * 100 / _total);
        if (p != _progress) {
            _progress = p;
            if (p % 20 == 0) LOG_I(TAG, "升级进度 %u%%", p);
        }
    }
    return true;
}

bool OtaService::uploadEnd(bool aborted) {
    if (_phase != OTA_RUNNING) return false;

    if (aborted) {
        Update.abort();
        _phase = OTA_FAILED;
        setMessage("升级被中断");
        LOG_E(TAG, "%s", _message);
        return false;
    }

    if (!Update.end(true)) {
        _phase = OTA_FAILED;
        setMessage("校验失败: %s", Update.errorString());
        LOG_E(TAG, "%s", _message);
        return false;
    }

    _phase    = OTA_SUCCESS;
    _progress = 100;
    setMessage("升级成功，共 %u 字节，即将重启", (unsigned)_written);
    LOG_W(TAG, "%s", _message);
    scheduleReboot(2000);
    return true;
}

// ---------------------------------------------------------------------------
// 远程 URL 升级
// ---------------------------------------------------------------------------
bool OtaService::startUrlUpdate(const String& url) {
    if (busy()) { setMessage("已有升级任务正在进行"); return false; }
    if (!WiFiService::instance().staOnline()) {
        setMessage("设备未联网，无法远程升级");
        return false;
    }
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        setMessage("升级地址必须以 http:// 或 https:// 开头");
        return false;
    }
    _pendingUrl = url;
    setMessage("已排队，即将开始下载…");
    LOG_I(TAG, "已接收远程升级请求: %s", url.c_str());
    return true;
}

void OtaService::doUrlUpdate(const String& url) {
    if (_beforeCb) _beforeCb();

    _phase    = OTA_RUNNING;
    _progress = 0;
    setMessage("正在下载固件…");
    LOG_W(TAG, "开始远程升级: %s", url.c_str());

    // 进度回调
    Update.onProgress([this](size_t done, size_t total) {
        if (!total) return;
        uint8_t p = (uint8_t)(done * 100 / total);
        if (p != _progress) {
            _progress = p;
            if (p % 10 == 0) LOG_I(TAG, "下载并写入 %u%%", p);
        }
    });

    httpUpdate.rebootOnUpdate(false);   // 重启时机由我们控制，便于返回结果给网页
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    t_httpUpdate_return ret;
    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();           // 不校验证书：多数自建固件服务器无有效证书
        client.setTimeout(15);
        ret = httpUpdate.update(client, url, FW_VERSION);
    } else {
        WiFiClient client;
        client.setTimeout(15);
        ret = httpUpdate.update(client, url, FW_VERSION);
    }

    switch (ret) {
        case HTTP_UPDATE_OK:
            _phase    = OTA_SUCCESS;
            _progress = 100;
            setMessage("远程升级成功，即将重启");
            LOG_W(TAG, "%s", _message);
            scheduleReboot(2000);
            break;
        case HTTP_UPDATE_NO_UPDATES:
            _phase = OTA_IDLE;
            setMessage("服务器提示已是最新版本");
            LOG_I(TAG, "%s", _message);
            break;
        default:
            _phase = OTA_FAILED;
            setMessage("远程升级失败(%d): %s",
                       httpUpdate.getLastError(),
                       httpUpdate.getLastErrorString().c_str());
            LOG_E(TAG, "%s", _message);
            break;
    }
}

bool OtaService::checkUpdate(String& outJson, String& err) {
    if (!CFG.sys.otaUrl.length()) { err = "未配置升级清单地址"; return false; }
    if (!WiFiService::instance().staOnline()) { err = "设备未联网"; return false; }

    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(5000);

    bool begun;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    if (CFG.sys.otaUrl.startsWith("https://")) {
        secureClient.setInsecure();
        begun = http.begin(secureClient, CFG.sys.otaUrl);
    } else {
        begun = http.begin(plainClient, CFG.sys.otaUrl);
    }
    if (!begun) { err = "无法解析升级清单地址"; return false; }

    http.addHeader("X-Device-Version", FW_VERSION);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        err = "获取清单失败, HTTP " + String(code);
        http.end();
        return false;
    }
    outJson = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, outJson)) { err = "清单格式不是合法 JSON"; return false; }
    LOG_I(TAG, "检查更新成功: %s", outJson.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void OtaService::tick() {
    // 排队的远程升级在这里执行（整个过程会阻塞，但此时设备本就该专心升级）
    if (_pendingUrl.length() && _phase != OTA_RUNNING) {
        String url = _pendingUrl;
        _pendingUrl = "";
        doUrlUpdate(url);
    }

    if (_rebootAtMs && (int32_t)(millis() - _rebootAtMs) >= 0) {
        _rebootAtMs = 0;
        LOG_W(TAG, "设备重启中…");
        Serial.flush();
        delay(100);
        ESP.restart();
    }
}

void OtaService::toJson(JsonObject root) {
    const char* phaseText[] = {"空闲", "升级中", "升级成功", "升级失败"};
    root["phase"]     = _phase;
    root["phaseText"] = phaseText[_phase <= OTA_FAILED ? _phase : 0];
    root["progress"]  = _progress;
    root["message"]   = _message;
    root["written"]   = _written;
    root["total"]     = _total;
    root["freeSpace"] = ESP.getFreeSketchSpace();
    root["version"]   = FW_VERSION;
    root["buildTime"] = FW_BUILD_TIME;
    root["checkUrl"]  = CFG.sys.otaUrl;
}
