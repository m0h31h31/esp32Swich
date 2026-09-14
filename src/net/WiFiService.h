/**
 * @file    WiFiService.h
 * @brief   WiFi 管理：AP / STA / AP+STA、DHCP 或静态地址、扫描、断线指数退避重连
 *
 * 稳定性设计：
 *  - 所有等待均非阻塞，主循环 tick() 驱动；
 *  - STA 连接失败按 15s/30s/60s/120s 退避重试，避免持续占用射频导致发热与丢包；
 *  - 连不上超过设定时长可自动开启 AP 应急热点（apFallback），保证永远有入口；
 *  - 使用事件回调而非轮询 status()，反应更快且不会漏事件。
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>
#include "app_config.h"

/// 连接阶段
enum WifiPhase : uint8_t {
    WP_IDLE = 0,       ///< 未启动
    WP_CONNECTING,     ///< STA 正在连接
    WP_CONNECTED,      ///< STA 已获取 IP
    WP_FAILED,         ///< 连接失败，等待退避重试
    WP_AP_ONLY,        ///< 仅热点模式
};

/// 扫描到的热点
struct ScanItem {
    String  ssid;
    String  bssid;
    int32_t rssi;
    uint8_t channel;
    uint8_t encryption;   ///< wifi_auth_mode_t
};

class WiFiService {
public:
    using StateCb = std::function<void(WifiPhase phase)>;

    static WiFiService& instance();

    void begin();
    void tick();

    /// 按当前配置重新应用（网页/蓝牙改完配置后调用）
    void applyConfig();

    /// 关闭 WiFi 省电，换取更低的网页/MQTT 响应延迟
    void applyPowerSave();

    /// 立即以给定参数尝试连接（不落盘，用于"测试连接"）
    bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs = WIFI_CONNECT_TIMEOUT);

    /// 手动开启/关闭应急热点
    void startAp(bool force = false);
    void stopAp();

    /**
     * 启动异步扫描（逐信道扫描，对已连接的热点客户端友好）。
     *
     * 为什么不用一次性全频段扫描：
     *  1) Arduino 内核把 scanComplete() 的内部超时设为 max_ms_per_chan*20（默认仅 6 秒），
     *     而 AP 模式下要兼顾伺候已连接客户端，全频段扫描常常超过这个值，直接被判失败；
     *  2) 全频段扫描期间射频连续跳 13 个信道，SoftAP 长时间离开自己的信道，手机会掉线。
     * 逐信道扫描每次只离开约 200ms，扫完立刻回到 AP 信道继续服务，两个问题都能规避。
     */
    bool startScan();
    bool scanning() const { return _scanning; }
    bool scanReady() const { return _scanReady; }
    const std::vector<ScanItem>& scanResult() const { return _scanResult; }
    /// 扫描进度百分比（0~100）
    uint8_t scanProgress() const;

    WifiPhase phase()     const { return _phase; }
    bool      staOnline() const { return WiFi.status() == WL_CONNECTED; }
    bool      apActive()  const { return _apActive; }
    int32_t   rssi()      const { return staOnline() ? WiFi.RSSI() : 0; }
    /// 信号质量 0~100
    uint8_t   signalQuality() const;
    String    ipString() const;

    void onStateChange(StateCb cb) { _cb = cb; }
    void toJson(JsonObject root);

    /// 累计断线次数（健康度指标）
    uint32_t disconnectCount() const { return _disconnectCount; }

    static const char* authName(uint8_t enc);

private:
    WiFiService() = default;

    void connectSta();
    void applyStaticIp();
    bool startChannelScan(uint8_t channel);   ///< 启动单个信道的扫描
    void collectChannelResult();              ///< 把本信道结果并入总表（按 SSID 去重取最强）
    void finishScan();
    void setPhase(WifiPhase p);
    void onEvent(arduino_event_id_t event, arduino_event_info_t info);

    WifiPhase _phase       = WP_IDLE;
    bool      _apActive    = false;
    bool      _staEnabled  = false;
    uint32_t  _connectStartMs = 0;
    uint32_t  _nextRetryMs = 0;
    uint16_t  _retryCount  = 0;
    uint32_t  _firstFailMs = 0;        ///< 首次连接失败时刻，用于 apFallback 计时
    uint32_t  _disconnectCount = 0;
    uint8_t   _lastDisconnReason = 0;
    uint32_t  _apCloseAtMs = 0;      ///< 联网成功后延迟关闭热点的时刻，0=不关

    bool      _scanning     = false;
    bool      _scanReady    = false;
    uint32_t  _scanStartMs  = 0;
    uint8_t   _scanChannel  = 0;     ///< 当前正在扫描的信道（1~13）
    uint8_t   _scanFailed   = 0;     ///< 本轮失败的信道数
    std::vector<ScanItem> _scanResult;

    StateCb   _cb = nullptr;
};
