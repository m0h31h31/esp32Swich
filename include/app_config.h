/**
 * @file    app_config.h
 * @brief   全局编译期常量与出厂默认值
 *
 * 说明：这里只放"编译期"的默认值与容量上限；所有运行期可改的参数
 *       都保存在 NVS 中，由 ConfigManager 统一管理。
 */
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// 固件标识
// ---------------------------------------------------------------------------
#ifndef FW_VERSION
#define FW_VERSION "1.2.0"
#endif
#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME "unknown"
#endif

#define FW_NAME         "ESP32-SmartSwitch"
#define FW_HW_NAME      "ESP32 Relay AC X1 v1.1"
#define CONFIG_VERSION  3           // 配置结构版本，升级时用于迁移

// ---------------------------------------------------------------------------
// 硬件默认引脚（均可在网页中修改）
// ---------------------------------------------------------------------------
#define DEF_RELAY_PIN         16    // 本板继电器驱动引脚
#define DEF_RELAY_ACTIVE_HIGH true  // 高电平吸合
#define DEF_BUTTON_PIN        0     // IO0 板载按键
#define DEF_BUTTON_ACTIVE_LOW true  // 按下为低
#define DEF_LED_PIN           -1    // 状态指示灯，-1 表示不使用

// 继电器引脚探测候选（网页"引脚探测"功能使用）
#define RELAY_PIN_CANDIDATES  {16, 32, 33, 25, 26, 27, 4, 5, 12, 13, 14, 23}

// ---------------------------------------------------------------------------
// 网络默认值
// ---------------------------------------------------------------------------
#define DEF_AP_PASSWORD    "12345678"
#define DEF_AP_IP          "192.168.4.1"
#define DEF_AP_CHANNEL     6
#define DEF_WEB_USER       "admin"
#define DEF_WEB_PASSWORD   "admin123"
#define DEF_WEB_PORT       80

// 巴法云（bemfa）默认接入点
#define DEF_MQTT_HOST      "bemfa.com"
#define DEF_MQTT_PORT      9501
#define DEF_MQTT_TOPIC     "switch001"
#define DEF_MQTT_KEEPALIVE 60

// NTP / 时区（CST-8 = 东八区，无夏令时）
#define DEF_NTP1           "ntp.aliyun.com"
#define DEF_NTP2           "ntp1.aliyun.com"
#define DEF_NTP3           "cn.pool.ntp.org"
#define DEF_TIMEZONE       "CST-8"

// ---------------------------------------------------------------------------
// 容量上限
// ---------------------------------------------------------------------------
#define MAX_MQTT_TOPICS    4        // 最多绑定的巴法云主题数
#define MAX_SCHEDULES      8        // 最多定时任务数
#define MAX_WEB_SESSIONS   4        // 最多同时在线的网页会话
#define LOG_RING_CAPACITY  80       // 日志环形缓冲条数
#define LOG_MSG_LEN        112      // 单条日志最大长度

// ---------------------------------------------------------------------------
// 行为参数
// ---------------------------------------------------------------------------
#define BUTTON_LONGPRESS_MS   10000 // IO0 长按恢复出厂设置
#define BUTTON_DEBOUNCE_MS    30
#define BUTTON_MULTI_GAP_MS   400   // 多击判定间隔
#define WIFI_RECONNECT_MS     15000 // STA 断线重连间隔（起始值，指数退避）
#define WIFI_CONNECT_TIMEOUT  20000 // 单次连接超时
#define MQTT_RECONNECT_MS     5000  // MQTT 重连间隔（起始值，指数退避）
#define TASK_WDT_TIMEOUT_S    30    // 任务看门狗超时
#define LOGIN_FAIL_LIMIT      5     // 连续登录失败次数上限
#define LOGIN_LOCK_MS         300000// 锁定时长 5 分钟

// NVS 命名空间
#define NVS_NS_CONFIG   "swcfg"
#define NVS_NS_STATE    "swstate"
