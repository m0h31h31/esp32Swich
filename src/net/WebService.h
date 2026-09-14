/**
 * @file    WebService.h
 * @brief   网页管理后台：登录认证 + REST API + 内置单页界面 + OTA 上传
 *
 * 安全措施：
 *  - 登录成功后发放随机会话令牌（Cookie + Header 双通道），带有效期；
 *  - 连续 5 次密码错误锁定 5 分钟，抵御暴力破解；
 *  - 所有写操作接口强制校验令牌；
 *  - 返回给前端的配置中密码字段一律以掩码替代，保存时掩码表示"不修改"。
 *
 * 稳定性措施：
 *  - 使用官方同步 WebServer，逻辑简单、内存可控；
 *  - 页面资源以 gzip 形式存放在 PROGMEM，不依赖文件系统，OTA 后不会丢失；
 *  - AP 模式下启用 DNS 强制门户，连上热点即自动弹出配置页。
 */
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include "app_config.h"

/// 一个登录会话
struct WebSession {
    char     token[33] = {0};
    uint32_t expireMs  = 0;
    uint32_t lastSeen  = 0;
    String   clientIp;
};

class WebService {
public:
    static WebService& instance();

    void begin();
    void tick();

    /// 端口变化后重启服务
    void restart();
    /// AP 模式变化时开关强制门户
    void updateCaptivePortal(bool apActive);

    uint16_t port() const { return _port; }
    bool     running() const { return _running; }
    uint32_t requestCount() const { return _reqCount; }

    void toJson(JsonObject root);

private:
    WebService() = default;

    // ---- 路由注册 ----
    void registerRoutes();

    // ---- 认证 ----
    bool   checkAuth();                 ///< 校验令牌，失败时已自行回复 401
    String createSession();
    void   destroySession(const String& token);
    String tokenFromRequest();
    bool   isLocked() const;

    // ---- 通用回复 ----
    void sendJson(int code, JsonDocument& doc);
    void sendOk(const String& msg = "操作成功");
    void sendErr(int code, const String& msg);
    bool readBody(JsonDocument& doc);

    // ---- 各接口处理 ----
    void handleRoot();
    void handleLogin();
    void handleLogout();
    void handleStatus();
    void handleSysInfo();
    void handleLogs();
    void handleLogsClear();
    void handleConfigGet();
    void handleConfigPost();
    void handleConfigExport();
    void handleConfigImport();
    void handleRelay();
    void handleRelayProbe();
    void handleWifiScan();
    void handleWifiTest();
    void handleMqttReconnect();
    void handleMqttPublish();
    void handleTimeSync();
    void handleSchedules();
    void handleOtaStatus();
    void handleOtaUrl();
    void handleOtaCheck();
    void handleOtaUploadData();       ///< 分片上传回调
    void handleOtaUploadDone();       ///< 上传完成后的响应
    void handleReboot();
    void handleFactoryReset();
    void handleNotFound();

    WebServer*  _server   = nullptr;
    DNSServer*  _dns      = nullptr;
    bool        _running  = false;
    bool        _portal   = false;
    uint16_t    _port     = DEF_WEB_PORT;
    uint32_t    _reqCount = 0;

    WebSession _sessions[MAX_WEB_SESSIONS];
    uint8_t    _failCount  = 0;
    uint32_t   _lockUntilMs= 0;
    uint32_t   _loginCount = 0;

    bool       _otaAuthOk  = false;   ///< 上传开始时的鉴权结果
    bool       _rebootAfterReply = false;
    bool       _factoryAfterReply = false;
    bool       _restartAfterReply = false;
};
