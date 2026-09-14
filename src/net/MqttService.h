/**
 * @file    MqttService.h
 * @brief   巴法云（bemfa）MQTT 接入：订阅控制、状态上报、断线指数退避重连
 *
 * 巴法云约定：
 *  - 服务器 bemfa.com:9501，clientId = 用户私钥（32 位）
 *  - 主题名后三位数字表示设备类型（001 插座/开关、002 灯、003 窗帘 …）
 *  - 下行消息 "on" / "off"；带 "#" 的复合消息取第一段（如 "on#50"）
 *  - 设备上报状态同样 publish 到该主题
 */
#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>
#include "app_config.h"

/// 连接状态
enum MqttPhase : uint8_t {
    MQ_DISABLED = 0,
    MQ_OFFLINE,        ///< 等待网络
    MQ_CONNECTING,
    MQ_CONNECTED,
    MQ_ERROR,          ///< 连接被拒绝，等待退避重试
};

/// 收到的一条云端指令
struct MqttCommand {
    String  topic;
    String  payload;
    uint8_t topicIndex;   ///< 对应配置中的第几个主题
};

class MqttService {
public:
    /// 云端下发开关指令回调：(主题下标, 期望状态)
    using SwitchCb  = std::function<void(uint8_t topicIndex, bool on)>;
    /// 原始消息回调（用于扩展设备类型）
    using MessageCb = std::function<void(const MqttCommand& cmd)>;

    static MqttService& instance();

    void begin();
    void tick();

    /// 配置变更后重连
    void applyConfig();
    /// 立即重连（网页"重新连接"按钮）
    void reconnectNow();
    /// 断开并停用
    void stop();

    /// 上报某个主题的开关状态
    bool publishState(uint8_t topicIndex, bool on);
    /// 上报所有绑定继电器的主题
    void publishAll(bool on);
    /// 发布自定义消息
    bool publish(const String& topic, const String& payload);

    bool      connected() const { return _client.connected(); }
    MqttPhase phase()     const { return _phase; }
    const char* phaseText() const;
    const char* lastErrorText() const { return _lastError; }

    void onSwitch(SwitchCb cb)   { _switchCb = cb; }
    void onMessage(MessageCb cb) { _msgCb = cb; }

    void toJson(JsonObject root);
    /// 输出主题卡片列表（含设备类型），供网页渲染控制卡片
    void topicsToJson(JsonArray arr);

private:
    MqttService() : _client(_net) {}

    void handleMessage(char* topic, uint8_t* payload, unsigned int len);
    bool doConnect();
    void subscribeAll();
    void setPhase(MqttPhase p);
    int8_t indexOfTopic(const String& topic) const;
    String reportTopic(const String& topic) const;

    WiFiClient   _net;
    // PubSubClient::connected() 未声明为 const，这里用 mutable 以便在 const 方法中查询状态
    mutable PubSubClient _client;

    MqttPhase _phase        = MQ_DISABLED;
    uint32_t  _nextRetryMs  = 0;
    uint16_t  _retryCount   = 0;
    uint32_t  _connectedMs  = 0;
    uint32_t  _lastReportMs = 0;
    uint32_t  _connectCount = 0;
    uint32_t  _rxCount      = 0;
    uint32_t  _txCount      = 0;
    uint32_t  _lastRxMs     = 0;
    char      _lastError[80]= {0};
    String    _lastMessage;
    bool      _states[MAX_MQTT_TOPICS] = {false};
    bool      _suppressEcho = false;   ///< 处理云端指令期间抑制回传，避免消息回环

    SwitchCb  _switchCb = nullptr;
    MessageCb _msgCb    = nullptr;
};
