/**
 * ============================================================================
 *  ESP32 智能开关  ——  主程序
 *
 *  硬件： ESP32 Relay AC X1 v1.1 （ESP32-WROOM-32E，继电器默认 IO16，按键 IO0）
 *
 *  功能总览：
 *    1. 出厂默认 AP 热点模式：连上热点即可在网页完成初始化配网
 *    2. 带登录认证的网页管理后台
 *    3. 设备状态展示：CPU 温度、信号强度、内存、芯片信息、运行日志
 *    4. 巴法云(bemfa) MQTT 接入、状态监控、设备重启
 *    5. NTP 联网校时
 *    6. DHCP / 静态 IP 配置
 *    7. 按巴法云主题类型自动生成对应控制卡片
 *    8. 在线 OTA 升级，配置跨版本兼容
 *    9. 网页配置继电器引脚与高低电平
 *   10. IO0 按键：单击切换 / 双击开热点 / 三击重启 / 长按 10 秒恢复出厂
 *   11. 看门狗、内存监控、断线自愈、定时任务、配置备份等健壮性增强
 *
 *  架构说明：
 *    各功能封装为独立模块（单例），主循环统一非阻塞驱动，模块间通过回调解耦。
 *    全部服务都在 loopTask 中轮询，不跨任务操作 WiFi 协议栈，避免偶发崩溃。
 * ============================================================================
 */

#include <Arduino.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>

#include "app_config.h"
#include "core/Logger.h"
#include "core/ConfigManager.h"
#include "core/SysInfo.h"
#include "io/RelayController.h"
#include "io/ButtonHandler.h"
#include "io/StatusLed.h"
#include "net/WiFiService.h"
#include "net/TimeService.h"
#include "net/MqttService.h"
#include "net/WebService.h"
#include "net/OtaService.h"
#include "app/ScheduleService.h"

static const char* TAG = "MAIN";

// 运行期状态
static bool     g_mdnsStarted   = false;
static bool     g_lastApActive  = false;
static uint32_t g_lastHealthMs  = 0;
static uint32_t g_lowHeapCount  = 0;
static uint32_t g_bootMs        = 0;

// ---------------------------------------------------------------------------
// 看门狗
// ---------------------------------------------------------------------------
static void setupWatchdog() {
    uint16_t sec = CFG.sys.wdtS;
    if (!sec) {
        LOG_W(TAG, "任务看门狗已被配置为关闭");
        return;
    }
    esp_task_wdt_init(sec, true);     // 超时后触发 panic 重启
    esp_task_wdt_add(NULL);           // 把主循环任务纳入监控
    LOG_I(TAG, "任务看门狗已启用，超时 %u 秒", sec);
}

// ---------------------------------------------------------------------------
// mDNS：让局域网内可以用 http://主机名.local 访问
// ---------------------------------------------------------------------------
static void startMdns() {
    if (g_mdnsStarted || !CFG.sys.mdns) return;
    if (!WiFiService::instance().staOnline()) return;

    String host = ConfigManager::instance().effectiveHostname();
    if (MDNS.begin(host.c_str())) {
        MDNS.addService("http", "tcp", WebService::instance().port());
        MDNS.addServiceTxt("http", "tcp", "device", "esp32-smart-switch");
        MDNS.addServiceTxt("http", "tcp", "version", FW_VERSION);
        g_mdnsStarted = true;
        LOG_I(TAG, "mDNS 已启动：http://%s.local", host.c_str());
    } else {
        LOG_W(TAG, "mDNS 启动失败");
    }
}

// ---------------------------------------------------------------------------
// 指示灯状态映射
// ---------------------------------------------------------------------------
static void updateLedMode() {
    if (!CFG.led.enable) return;
    StatusLed& led = StatusLed::instance();

    if (CFG.led.followRelay) {
        led.setMode(RelayController::instance().state() ? LM_ON : LM_OFF);
        return;
    }

    WiFiService& w = WiFiService::instance();
    if (w.apActive() && !w.staOnline())        led.setMode(LM_FAST_BLINK);   // 配网中
    else if (w.phase() == WP_CONNECTING)       led.setMode(LM_SLOW_BLINK);   // 连接中
    else if (!w.staOnline())                   led.setMode(LM_SLOW_BLINK);
    else if (CFG.mqtt.enable && !MqttService::instance().connected())
                                               led.setMode(LM_DOUBLE_BLINK); // 云端断开
    else                                       led.setMode(LM_HEARTBEAT);    // 一切正常
}

// ---------------------------------------------------------------------------
// 健康巡检：内存、定期重启
// ---------------------------------------------------------------------------
static void healthCheck() {
    uint32_t now = millis();
    if (now - g_lastHealthMs < 30000) return;
    g_lastHealthMs = now;

    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlk   = ESP.getMaxAllocHeap();

    // 内存告急：连续多次低于阈值则主动重启，好过等着崩溃
    if (freeHeap < 20000 || maxBlk < 12000) {
        g_lowHeapCount++;
        LOG_E(TAG, "内存不足告警 (%u/%u)：可用 %u B，最大连续块 %u B",
              g_lowHeapCount, 5, freeHeap, maxBlk);
        if (g_lowHeapCount >= 5) {
            LOG_E(TAG, "内存持续不足，主动重启自愈");
            OtaService::instance().scheduleReboot(500);
        }
    } else if (g_lowHeapCount) {
        LOG_I(TAG, "内存已恢复正常 (%u B)", freeHeap);
        g_lowHeapCount = 0;
    }

    // 定期自动重启（长期运行的稳妥做法，可在网页关闭）
    if (CFG.sys.autoRebootH) {
        uint32_t limit = CFG.sys.autoRebootH * 3600UL;
        if (SysInfo::uptimeSeconds() >= limit) {
            LOG_W(TAG, "已运行 %u 小时，执行计划内重启", CFG.sys.autoRebootH);
            OtaService::instance().scheduleReboot(1000);
        }
    }
}

// ---------------------------------------------------------------------------
// 各模块回调接线
// ---------------------------------------------------------------------------
static void wireCallbacks() {
    RelayController& relay = RelayController::instance();
    ButtonHandler&   btn   = ButtonHandler::instance();
    MqttService&     mqtt  = MqttService::instance();
    WiFiService&     wifi  = WiFiService::instance();
    StatusLed&       led   = StatusLed::instance();

    // 继电器状态变化 -> 上报巴法云 + 指示灯
    relay.onChange([&mqtt, &led](bool state, RelaySource src) {
        if (CFG.mqtt.enable && CFG.mqtt.reportOnChange && src != RS_BOOT)
            mqtt.publishAll(state);
        if (CFG.led.enable && CFG.led.followRelay) led.setMode(state ? LM_ON : LM_OFF);
    });

    // 巴法云下发开关指令 -> 驱动继电器
    mqtt.onSwitch([&relay](uint8_t idx, bool on) {
        relay.set(on, RS_MQTT, true);   // 云端指令不受最小间隔限制
    });

    // WiFi 状态变化 -> 校时 / 强制门户 / mDNS
    wifi.onStateChange([](WifiPhase phase) {
        if (phase == WP_CONNECTED) {
            TimeService::instance().requestSync();
            startMdns();
        }
    });

    // ---- 按键功能 ----
    btn.onSingleClick([&relay, &led]() {
        relay.toggle(RS_BUTTON);
        led.blink(1, 60, 60);
    });

    btn.onDoubleClick([&wifi, &led]() {
        LOG_W(TAG, "双击：开启应急热点（忘记密码或换了路由器时的补救入口）");
        wifi.startAp(true);
        led.blink(2, 90, 90);
    });

    btn.onTripleClick([&led]() {
        LOG_W(TAG, "三击：重启设备");
        led.blink(3, 70, 70);
        OtaService::instance().scheduleReboot(600);
    });

    btn.onLongPress([&led]() {
        LOG_E(TAG, "长按 %u 秒：执行恢复出厂设置", CFG.button.longPressMs / 1000);
        led.blink(6, 60, 60);
        ConfigManager::instance().factoryReset();
        delay(400);
        ESP.restart();
    });

    // 长按进度：进度过半时用快闪提示用户"再按下去就要清空了"
    btn.onLongProgress([&led](uint8_t pct) {
        if (!CFG.led.enable) return;
        if (pct >= 50)      led.setMode(LM_FAST_BLINK);
        else if (pct == 0)  updateLedMode();
    });

    // OTA 开始前停掉巴法云，把内存和射频让给升级过程
    OtaService::instance().onBeforeUpdate([]() {
        LOG_W(TAG, "升级即将开始：暂停巴法云连接");
        MqttService::instance().stop();
    });
}

// ===========================================================================
// setup
// ===========================================================================
void setup() {
    g_bootMs = millis();

    // 1) 日志与基础信息
    Logger::begin(115200, LL_INFO);
    SysInfo::begin();
    LOG_I(TAG, "固件 %s  编译于 %s", FW_VERSION, FW_BUILD_TIME);
    LOG_I(TAG, "硬件 %s  芯片 %s  复位原因: %s",
          FW_HW_NAME, ESP.getChipModel(), SysInfo::resetReasonText());
    LOG_I(TAG, "Flash %s  可用堆 %u B",
          SysInfo::formatBytes(ESP.getFlashChipSize()).c_str(), ESP.getFreeHeap());

    // 2) 配置（必须最先加载，后续模块都依赖它）
    ConfigManager::instance().begin();
    Logger::setLevel((LogLevel)CFG.sys.logLevel);
    LOG_I(TAG, "设备名称: %s", CFG.sys.deviceName.c_str());

    // 3) 本地 IO（先于网络，保证断网也能用按键控制）
    RelayController::instance().begin();
    ButtonHandler::instance().begin();
    StatusLed::instance().begin();

    // 4) 回调接线
    wireCallbacks();

    // 5) 网络与服务
    WiFiService::instance().begin();
    TimeService::instance().begin();
    MqttService::instance().begin();
    OtaService::instance().begin();
    WebService::instance().begin();
    ScheduleService::instance().begin();

    // 6) 看门狗（放在最后，避免初始化耗时被误判为卡死）
    setupWatchdog();

    LOG_I(TAG, "系统启动完成，耗时 %lu ms，剩余堆 %u B",
          (unsigned long)(millis() - g_bootMs), ESP.getFreeHeap());

    if (!ConfigManager::instance().hasWifiCredential())
        LOG_W(TAG, "尚未配置 WiFi —— 请手机连接热点 \"%s\"，浏览器打开 http://%s 完成初始化",
              ConfigManager::instance().effectiveApSsid().c_str(), DEF_AP_IP);
}

// ===========================================================================
// loop  —— 所有模块统一在这里非阻塞轮询
// ===========================================================================
void loop() {
    // 喂狗
    esp_task_wdt_reset();

    // 本地 IO：优先级最高，保证按键手感
    ButtonHandler::instance().tick();
    RelayController::instance().tick();
    StatusLed::instance().tick();

    // 网络与服务
    WiFiService::instance().tick();
    WebService::instance().tick();
    TimeService::instance().tick();
    MqttService::instance().tick();
    OtaService::instance().tick();
    ScheduleService::instance().tick();

    // AP 状态变化时同步强制门户
    bool ap = WiFiService::instance().apActive();
    if (ap != g_lastApActive) {
        g_lastApActive = ap;
        WebService::instance().updateCaptivePortal(ap);
    }

    // 联网后补启 mDNS（首次连接可能早于 Web 服务就绪）
    if (!g_mdnsStarted) startMdns();

    // 指示灯与健康巡检（内部自带节流）
    static uint32_t lastLed = 0;
    if (millis() - lastLed > 500) { lastLed = millis(); updateLedMode(); }
    healthCheck();

    // 让出 CPU 给 WiFi 协议栈，显著降低发热与丢包
    delay(2);
}
