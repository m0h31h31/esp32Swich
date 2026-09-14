/**
 * @file    OtaService.h
 * @brief   在线升级：网页上传 .bin + 远程 URL 升级 + 版本检查
 *
 * 安全策略：
 *  - 升级前校验固件头魔数 0xE9，拒绝明显非法的文件；
 *  - 校验剩余分区空间，空间不足直接拒绝，避免写坏；
 *  - 升级期间暂停 MQTT 连接，降低内存与射频压力；
 *  - 写入失败自动 abort，原固件不受影响（双分区 OTA，失败仍从旧分区启动）；
 *  - 升级成功后配置保留在 NVS 中，新固件按"缺字段取默认"的规则平滑兼容。
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

/// 升级阶段
enum OtaPhase : uint8_t {
    OTA_IDLE = 0,
    OTA_RUNNING,
    OTA_SUCCESS,
    OTA_FAILED,
};

class OtaService {
public:
    static OtaService& instance();

    void begin();
    void tick();

    // ---- 网页上传（配合 WebServer 的分片上传回调）----
    bool uploadBegin(const String& filename, size_t totalSize);
    bool uploadWrite(const uint8_t* data, size_t len);
    bool uploadEnd(bool aborted);

    // ---- 远程 URL 升级 ----
    /// 触发一次异步 URL 升级（实际执行在 tick 中，避免阻塞 HTTP 响应）
    bool startUrlUpdate(const String& url);

    /// 检查更新：请求配置中的清单地址，返回 JSON {version,url,notes,size}
    bool checkUpdate(String& outJson, String& err);

    OtaPhase    phase()    const { return _phase; }
    uint8_t     progress() const { return _progress; }
    const char* message()  const { return _message; }
    bool        busy()     const { return _phase == OTA_RUNNING || _pendingUrl.length(); }

    void toJson(JsonObject root);

    /// 升级完成后由主程序调用，延时重启
    void scheduleReboot(uint32_t delayMs = 1500);
    bool rebootPending() const { return _rebootAtMs != 0; }

    /// 注册"升级开始"回调，用于让其他模块让出资源
    void onBeforeUpdate(std::function<void()> cb) { _beforeCb = cb; }

private:
    OtaService() = default;
    void setMessage(const char* fmt, ...);
    void doUrlUpdate(const String& url);

    OtaPhase _phase      = OTA_IDLE;
    uint8_t  _progress   = 0;
    size_t   _total      = 0;
    size_t   _written    = 0;
    char     _message[96]= {0};
    uint32_t _rebootAtMs = 0;
    String   _pendingUrl;
    bool     _headerChecked = false;
    std::function<void()> _beforeCb = nullptr;
};
