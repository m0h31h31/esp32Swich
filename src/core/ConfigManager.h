/**
 * @file    ConfigManager.h
 * @brief   配置中心：所有可持久化参数的定义、加载、保存、迁移
 *
 * 兼容性策略（对应需求"升级后配置继续兼容"）：
 *  - 配置以 JSON 文本存于 NVS，而非结构体二进制，字段增删不会破坏旧数据；
 *  - 加载时先填出厂默认值，再逐字段覆盖，旧固件没有的新字段自动取默认值；
 *  - 带 version 字段，跨版本可执行 migrate() 做字段改名/单位换算；
 *  - 保存前做完整性校验，写入后回读校验，失败自动回滚到备份槽。
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "app_config.h"
#include "core/Logger.h"

// ---------------------------------------------------------------------------
// 各配置分组
// ---------------------------------------------------------------------------

/// WiFi 工作模式
enum WifiMode : uint8_t {
    WM_AP     = 0,   ///< 仅热点
    WM_STA    = 1,   ///< 仅连接路由器
    WM_APSTA  = 2,   ///< 双模
};

/// STA（连接路由器）参数
struct StaConfig {
    String   ssid;
    String   password;
    bool     dhcp    = true;    ///< true=自动获取；false=静态地址
    String   ip;
    String   gateway;
    String   netmask;
    String   dns1;
    String   dns2;
    bool     bssidLock = false; ///< 是否锁定到指定 BSSID（多 AP 环境防漫游抖动）
    String   bssid;
};

/// AP（热点）参数
struct ApConfig {
    String   ssid;                          ///< 为空则自动使用 设备名-MAC 后四位
    String   password = DEF_AP_PASSWORD;
    uint8_t  channel  = DEF_AP_CHANNEL;
    bool     hidden   = false;
    uint8_t  maxConn  = 4;
    String   ip       = DEF_AP_IP;
};

struct WifiConfig {
    uint8_t   mode       = WM_AP;   ///< 出厂默认开热点，便于首次网页初始化
    StaConfig sta;
    ApConfig  ap;
    bool      apFallback = true;   ///< STA 连不上时自动开热点便于补救
    uint16_t  apFallbackS = 60;    ///< 连不上多少秒后开热点
    int8_t    txPower    = 0;      ///< 0=默认，其余为 dBm 档位（弱信号环境可调）
};

/// 网页服务
struct WebConfig {
    String   username = DEF_WEB_USER;
    String   password = DEF_WEB_PASSWORD;
    uint16_t port     = DEF_WEB_PORT;
    uint16_t sessionMin = 120;     ///< 会话有效期（分钟）
    bool     allowLan   = true;    ///< 是否允许非本机访问（保留位）
};

/// 单个巴法云主题绑定
struct TopicConfig {
    bool     enable = false;
    String   topic;                ///< 巴法云主题名，如 switch001
    String   name;                 ///< 显示名称
    uint8_t  bind   = 0;           ///< 0=不绑定(仅展示) 1=绑定继电器
    uint8_t  type   = 1;           ///< 由主题后缀推断的设备类型，见 bafaTypeOf()
};

/// MQTT / 巴法云
struct MqttConfig {
    bool     enable    = false;
    String   host      = DEF_MQTT_HOST;
    uint16_t port      = DEF_MQTT_PORT;
    String   key;                  ///< 巴法云私钥（同时作为 clientId）
    String   clientSuffix;         ///< 多设备共用一个 key 时的区分后缀
    uint16_t keepalive = DEF_MQTT_KEEPALIVE;
    bool     reportOnChange = true;///< 状态变化时主动上报
    uint16_t reportIntervalS = 60; ///< 周期上报间隔，0=关闭
    bool     syncOnConnect  = true;///< 连接成功后同步一次状态
    /**
     * 状态上报使用的主题后缀（巴法云 MQTT 协议规定）：
     *   0 = "/up"  只更新云端数据、不推送 —— 设备上报状态的标准做法（默认）
     *   1 = "/set" 推送给该主题的所有订阅者（推送者自己不会收到）
     *   2 = 无后缀（兼容旧版接入方式）
     */
    uint8_t  reportSuffix = 0;
    uint8_t  topicCount = 1;
    TopicConfig topics[MAX_MQTT_TOPICS];
};

/// 继电器
struct RelayConfig {
    int8_t   pin        = DEF_RELAY_PIN;
    bool     activeHigh = DEF_RELAY_ACTIVE_HIGH;
    uint8_t  powerOn    = 2;       ///< 上电默认：0=关 1=开 2=恢复断电前
    uint16_t minIntervalMs = 300;  ///< 两次动作最小间隔，保护继电器
    uint32_t autoOffS   = 0;       ///< 开启后自动关闭（秒），0=不自动关
};

/// 按键
struct ButtonConfig {
    bool     enable     = true;
    int8_t   pin        = DEF_BUTTON_PIN;
    bool     activeLow  = DEF_BUTTON_ACTIVE_LOW;
    uint16_t debounceMs = BUTTON_DEBOUNCE_MS;
    uint16_t longPressMs= BUTTON_LONGPRESS_MS;
    bool     multiClick = true;    ///< 是否启用双击/三击功能
};

/// 状态指示灯
struct LedConfig {
    bool    enable     = false;
    int8_t  pin        = DEF_LED_PIN;
    bool    activeHigh = true;
    bool    followRelay= false;    ///< true=跟随继电器状态，false=指示网络状态
};

/// 时间同步
struct TimeConfig {
    bool     enable   = true;
    String   ntp1     = DEF_NTP1;
    String   ntp2     = DEF_NTP2;
    String   ntp3     = DEF_NTP3;
    String   tz       = DEF_TIMEZONE;
    uint32_t intervalS= 3600;      ///< 重新校时间隔
};

/// 定时任务
struct ScheduleItem {
    bool    enable = false;
    uint8_t days   = 0x7F;         ///< bit0=周日 ... bit6=周六
    uint8_t hour   = 0;
    uint8_t minute = 0;
    uint8_t action = 0;            ///< 0=关 1=开 2=翻转
};

/// 系统
struct SysConfig {
    String   deviceName = "智能开关";
    String   hostname;             ///< 为空则自动生成 esp32sw-xxxx
    uint8_t  logLevel   = LL_INFO;
    uint32_t autoRebootH= 0;       ///< 定期自动重启（小时），0=禁用
    bool     mdns       = true;
    uint16_t wdtS       = TASK_WDT_TIMEOUT_S;
    String   otaUrl;               ///< 在线升级清单地址（可选）
    bool     otaAutoCheck = false;
};

/// 完整配置
struct AppConfig {
    uint16_t     version = CONFIG_VERSION;
    SysConfig    sys;
    WifiConfig   wifi;
    WebConfig    web;
    MqttConfig   mqtt;
    RelayConfig  relay;
    ButtonConfig button;
    LedConfig    led;
    TimeConfig   time;
    ScheduleItem schedules[MAX_SCHEDULES];
};

// ---------------------------------------------------------------------------
// 配置管理器（单例）
// ---------------------------------------------------------------------------
class ConfigManager {
public:
    static ConfigManager& instance();

    /// 从 NVS 加载；不存在或损坏时写入出厂默认值
    bool begin();

    /// 保存到 NVS（带回读校验）
    bool save();

    /// 恢复出厂设置（清空 NVS 并重置内存副本）
    void factoryReset();

    AppConfig&       cfg()       { return _cfg; }
    const AppConfig& cfg() const { return _cfg; }

    /// 是否仍在使用默认网页密码（用于提示用户修改）
    bool isDefaultWebPassword() const;

    /// 是否已配置过 WiFi（决定上电后是否直接进入配网模式）
    bool hasWifiCredential() const;

    /// 设备默认名/主机名（基于 MAC 后四位）
    static String macSuffix();
    String effectiveHostname() const;
    String effectiveApSsid() const;
    String effectiveMqttClientId() const;

    /// 序列化：includeSecret=false 时密码字段以 "" 返回，用于网页展示
    void toJson(JsonObject root, bool includeSecret) const;

    /**
     * 反序列化（增量更新）：只更新 JSON 中出现的字段
     * @param section 只更新指定分组；为空则按 root 内的分组键更新
     * @return 是否有字段被修改
     */
    bool applyJson(JsonObjectConst root, String& err);

    /// 导出为完整 JSON 文本（备份用，含密文）
    String exportJson() const;
    /// 从备份文本导入
    bool importJson(const String& text, String& err);

    /// 根据巴法云主题名后缀推断设备类型
    /// 1=开关 2=插座 3=灯 4=传感器 5=窗帘 6=空调 7=风扇 8=电视 0=未知
    static uint8_t bafaTypeOf(const String& topic);
    static const char* bafaTypeName(uint8_t type);

private:
    ConfigManager() = default;
    AppConfig _cfg;
    bool      _loaded = false;

    void setDefaults();
    void migrate(uint16_t fromVersion);
    bool writeNvs(const String& json);
    String readNvs();
};

/// 便捷访问宏
#define CFG (ConfigManager::instance().cfg())
