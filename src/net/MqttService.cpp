#include "net/MqttService.h"
#include "net/WiFiService.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "app_config.h"

static const char* TAG = "MQTT";

MqttService& MqttService::instance() {
    static MqttService inst;
    return inst;
}

const char* MqttService::phaseText() const {
    switch (_phase) {
        case MQ_DISABLED:   return "未启用";
        case MQ_OFFLINE:    return "等待联网";
        case MQ_CONNECTING: return "连接中";
        case MQ_CONNECTED:  return "已连接";
        default:            return "连接失败";
    }
}

/// PubSubClient 错误码转中文，便于用户自助排查
static const char* mqttStateText(int state) {
    switch (state) {
        case -4: return "服务器无响应（超时）";
        case -3: return "网络连接被断开";
        case -2: return "TCP 连接失败，请检查地址/端口";
        case -1: return "客户端主动断开";
        case  0: return "已连接";
        case  1: return "协议版本不被支持";
        case  2: return "客户端 ID 被拒绝（请检查私钥是否正确/是否被顶号）";
        case  3: return "服务器不可用";
        case  4: return "用户名或密码错误";
        case  5: return "未授权";
        default: return "未知错误";
    }
}

void MqttService::setPhase(MqttPhase p) {
    if (_phase == p) return;
    _phase = p;
}

int8_t MqttService::indexOfTopic(const String& topic) const {
    // 巴法云下行消息的主题名可能带 /set 之类的后缀，这里先剥掉再比对
    String base = topic;
    int slash = base.indexOf('/');
    if (slash > 0) base = base.substring(0, slash);

    for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++)
        if (CFG.mqtt.topics[i].topic == base) return (int8_t)i;
    return -1;
}

/**
 * 拼出上报用的主题名。
 *
 * 巴法云 MQTT 协议规定（见官方文档"set 指令 / up 指令"）：
 *   topic/up   只更新云端数据、不做任何推送 —— 设备上报自身状态的标准做法
 *   topic/set  推送给该主题的所有订阅者（推送者自己收不到，天然不会回环）
 *   topic      旧版接入方式，保留以兼容老配置
 * 控制台与手机 App 显示的开关状态，取的就是云端数据，所以默认用 /up。
 */
String MqttService::reportTopic(const String& topic) const {
    switch (CFG.mqtt.reportSuffix) {
        case 1:  return topic + "/set";
        case 2:  return topic;
        default: return topic + "/up";
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void MqttService::begin() {
    _client.setCallback([this](char* t, uint8_t* p, unsigned int l) {
        handleMessage(t, p, l);
    });
    // 缩短 socket 超时，避免 connect() 长时间阻塞主循环触发看门狗
    _client.setSocketTimeout(4);
    applyConfig();
}

void MqttService::applyConfig() {
    if (_client.connected()) _client.disconnect();

    if (!CFG.mqtt.enable) {
        setPhase(MQ_DISABLED);
        LOG_I(TAG, "巴法云功能未启用");
        return;
    }
    if (CFG.mqtt.key.length() < 8) {
        setPhase(MQ_ERROR);
        snprintf(_lastError, sizeof(_lastError), "未配置私钥");
        LOG_W(TAG, "未配置巴法云私钥，暂不连接");
        return;
    }

    _client.setServer(CFG.mqtt.host.c_str(), CFG.mqtt.port);
    _client.setKeepAlive(CFG.mqtt.keepalive);
    _client.setBufferSize(512);

    _retryCount  = 0;
    _nextRetryMs = 0;
    setPhase(MQ_OFFLINE);
    LOG_I(TAG, "巴法云配置: %s:%u, %u 个主题",
          CFG.mqtt.host.c_str(), CFG.mqtt.port, CFG.mqtt.topicCount);
}

void MqttService::reconnectNow() {
    if (!CFG.mqtt.enable) return;
    if (_client.connected()) _client.disconnect();
    _retryCount  = 0;
    _nextRetryMs = 0;
    setPhase(MQ_OFFLINE);
    LOG_I(TAG, "手动触发重新连接");
}

void MqttService::stop() {
    if (_client.connected()) _client.disconnect();
    setPhase(MQ_DISABLED);
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------
bool MqttService::doConnect() {
    String clientId = ConfigManager::instance().effectiveMqttClientId();
    setPhase(MQ_CONNECTING);
    LOG_I(TAG, "正在连接 %s:%u (第 %u 次)…",
          CFG.mqtt.host.c_str(), CFG.mqtt.port, _retryCount + 1);

    // 巴法云仅校验 clientId（私钥），用户名/密码留空即可
    bool ok = _client.connect(clientId.c_str());
    if (ok) {
        _connectedMs  = millis();
        _connectCount++;
        _retryCount   = 0;
        _lastError[0] = '\0';
        setPhase(MQ_CONNECTED);
        LOG_I(TAG, "巴法云连接成功");
        subscribeAll();
        // 订阅成功不代表云端存在该主题：巴法云要求"先在 MQTT 控制台新建主题"，
        // 未创建的主题订阅同样返回成功，但控制台里不会出现这台设备
        LOG_I(TAG, "提示：若控制台看不到设备，请确认主题已在巴法云 MQTT 控制台新建");
        if (CFG.mqtt.syncOnConnect) {
            // 连接后立即上报一次当前状态，让云端与设备保持一致
            for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++)
                if (CFG.mqtt.topics[i].enable && CFG.mqtt.topics[i].bind == 1)
                    publishState(i, _states[i]);
        }
        return true;
    }

    int st = _client.state();
    snprintf(_lastError, sizeof(_lastError), "%s", mqttStateText(st));
    LOG_W(TAG, "连接失败: %s (code %d)", _lastError, st);
    setPhase(MQ_ERROR);
    return false;
}

void MqttService::subscribeAll() {
    for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++) {
        const TopicConfig& t = CFG.mqtt.topics[i];
        if (!t.enable || !t.topic.length()) continue;
        if (_client.subscribe(t.topic.c_str()))
            LOG_I(TAG, "已订阅主题 %s (%s)", t.topic.c_str(),
                  ConfigManager::bafaTypeName(t.type));
        else
            LOG_W(TAG, "订阅主题 %s 失败", t.topic.c_str());
    }
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
void MqttService::handleMessage(char* topic, uint8_t* payload, unsigned int len) {
    if (len > 200) len = 200;
    String msg;
    msg.reserve(len + 1);
    for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
    msg.trim();

    String tp = String(topic);
    _rxCount++;
    _lastRxMs    = millis();
    _lastMessage = tp + " => " + msg;
    LOG_I(TAG, "收到云端消息 [%s] %s", topic, msg.c_str());

    int8_t idx = indexOfTopic(tp);
    if (idx < 0) {
        LOG_W(TAG, "主题 %s 未在配置中，忽略", topic);
        return;
    }

    MqttCommand cmd{tp, msg, (uint8_t)idx};
    if (_msgCb) _msgCb(cmd);

    // 巴法云复合指令形如 "on#3#50"，取第一段作为开关动作
    String action = msg;
    int hashPos = action.indexOf('#');
    if (hashPos >= 0) action = action.substring(0, hashPos);
    action.toLowerCase();

    bool on;
    if      (action == "on"  || action == "1" || action == "open")  on = true;
    else if (action == "off" || action == "0" || action == "close") on = false;
    else {
        LOG_W(TAG, "无法识别的指令 \"%s\"，已忽略", msg.c_str());
        return;
    }

    _states[idx] = on;
    if (CFG.mqtt.topics[idx].bind == 1 && _switchCb) {
        _suppressEcho = true;        // 本次状态变化由云端发起，不再回传，避免回环
        _switchCb(idx, on);
        _suppressEcho = false;
    }
}

// ---------------------------------------------------------------------------
// 上报
// ---------------------------------------------------------------------------
bool MqttService::publish(const String& topic, const String& payload) {
    if (!_client.connected()) return false;
    bool ok = _client.publish(topic.c_str(), payload.c_str());
    if (ok) _txCount++;
    else    LOG_W(TAG, "消息发送失败 [%s]", topic.c_str());
    return ok;
}

bool MqttService::publishState(uint8_t topicIndex, bool on) {
    if (topicIndex >= CFG.mqtt.topicCount) return false;
    const TopicConfig& t = CFG.mqtt.topics[topicIndex];
    if (!t.enable || !t.topic.length()) return false;

    _states[topicIndex] = on;
    if (!_client.connected()) return false;

    String pubTopic = reportTopic(t.topic);
    bool ok = publish(pubTopic, on ? "on" : "off");
    if (ok) LOG_I(TAG, "已上报 [%s] %s", pubTopic.c_str(), on ? "on" : "off");
    return ok;
}

void MqttService::publishAll(bool on) {
    // 只有用"无后缀"的旧版上报方式时才可能出现消息回环，需要抑制回传；
    // /up 不推送、/set 不回推给发送者本身，这两种情况都应该正常上报，
    // 否则云端状态会与设备不一致。
    if (_suppressEcho && CFG.mqtt.reportSuffix == 2) {
        for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++)
            if (CFG.mqtt.topics[i].bind == 1) _states[i] = on;
        return;
    }
    for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++)
        if (CFG.mqtt.topics[i].enable && CFG.mqtt.topics[i].bind == 1)
            publishState(i, on);
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void MqttService::tick() {
    if (!CFG.mqtt.enable) {
        if (_phase != MQ_DISABLED) setPhase(MQ_DISABLED);
        return;
    }
    if (CFG.mqtt.key.length() < 8) return;

    if (!WiFiService::instance().staOnline()) {
        if (_phase == MQ_CONNECTED) {
            LOG_W(TAG, "网络断开，MQTT 转入等待");
            _client.disconnect();
        }
        setPhase(MQ_OFFLINE);
        return;
    }

    uint32_t now = millis();

    if (!_client.connected()) {
        if (_phase == MQ_CONNECTED) {
            LOG_W(TAG, "与巴法云的连接已断开");
            setPhase(MQ_OFFLINE);
        }
        if ((int32_t)(now - _nextRetryMs) >= 0) {
            if (!doConnect()) {
                // 指数退避 5s → 10s → 20s → 40s → 60s（封顶）
                uint32_t backoff = MQTT_RECONNECT_MS * (1UL << (_retryCount > 3 ? 3 : _retryCount));
                if (backoff > 60000UL) backoff = 60000UL;
                _nextRetryMs = now + backoff;
                if (_retryCount < 250) _retryCount++;
            }
        }
        return;
    }

    _client.loop();

    // 周期上报，防止云端状态与设备不一致（弱网丢包后的兜底）
    if (CFG.mqtt.reportIntervalS &&
        now - _lastReportMs > CFG.mqtt.reportIntervalS * 1000UL) {
        _lastReportMs = now;
        for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++)
            if (CFG.mqtt.topics[i].enable && CFG.mqtt.topics[i].bind == 1)
                publishState(i, _states[i]);
    }
}

// ---------------------------------------------------------------------------
// 状态输出
// ---------------------------------------------------------------------------
void MqttService::toJson(JsonObject root) {
    root["enable"]      = CFG.mqtt.enable;
    root["phase"]       = _phase;
    root["phaseText"]   = phaseText();
    root["connected"]   = _client.connected();
    root["host"]        = CFG.mqtt.host;
    root["port"]        = CFG.mqtt.port;
    root["clientId"]    = CFG.mqtt.key.length() > 8
                          ? CFG.mqtt.key.substring(0, 4) + "****" +
                            CFG.mqtt.key.substring(CFG.mqtt.key.length() - 4)
                          : String("未配置");
    root["connectCount"]= _connectCount;
    root["onlineSec"]   = _client.connected() ? (millis() - _connectedMs) / 1000 : 0;
    root["rx"]          = _rxCount;
    root["tx"]          = _txCount;
    root["lastMessage"] = _lastMessage;
    root["lastRxAgo"]   = _lastRxMs ? (millis() - _lastRxMs) / 1000 : 0;
    root["lastError"]   = _lastError;
    root["retries"]     = _retryCount;
}

void MqttService::topicsToJson(JsonArray arr) {
    for (uint8_t i = 0; i < CFG.mqtt.topicCount; i++) {
        const TopicConfig& t = CFG.mqtt.topics[i];
        if (!t.topic.length()) continue;
        JsonObject o = arr.add<JsonObject>();
        o["index"]    = i;
        o["topic"]    = t.topic;
        o["name"]     = t.name;
        o["enable"]   = t.enable;
        o["bind"]     = t.bind;
        o["type"]     = t.type;
        o["typeName"] = ConfigManager::bafaTypeName(t.type);
        o["state"]    = _states[i];
    }
}
