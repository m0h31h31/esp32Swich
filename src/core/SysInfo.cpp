#include "core/SysInfo.h"
#include "core/ConfigManager.h"
#include "app_config.h"

#include <WiFi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <rom/rtc.h>

uint32_t SysInfo::_bootMs      = 0;
int      SysInfo::_resetReason = 0;

void SysInfo::begin() {
    _bootMs      = millis();
    _resetReason = (int)esp_reset_reason();
}

uint32_t SysInfo::uptimeSeconds() { return millis() / 1000; }

String SysInfo::uptimeString() {
    uint32_t s = uptimeSeconds();
    uint32_t d = s / 86400; s %= 86400;
    uint32_t h = s / 3600;  s %= 3600;
    uint32_t m = s / 60;    s %= 60;
    char buf[48];
    if (d) snprintf(buf, sizeof(buf), "%u天 %02u:%02u:%02u", (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)s);
    else   snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    return String(buf);
}

float SysInfo::cpuTemperature() {
    // ESP32（非 S2/S3/C3）内部温度传感器未经校准，仅作趋势参考
    float t = temperatureRead();
    if (isnan(t) || t < -40.0f || t > 150.0f) return NAN;
    return t;
}

String SysInfo::chipModel() {
    String m = ESP.getChipModel();
    // WROOM-32E 实际为 ESP32-D0WD-V3，补充封装信息便于识别
    m += "  Rev.";
    m += String(ESP.getChipRevision());
    return m;
}

String SysInfo::macAddress() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[20];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

const char* SysInfo::resetReasonText() {
    switch (_resetReason) {
        case ESP_RST_POWERON:  return "上电复位";
        case ESP_RST_EXT:      return "外部复位";
        case ESP_RST_SW:       return "软件重启";
        case ESP_RST_PANIC:    return "异常崩溃";
        case ESP_RST_INT_WDT:  return "中断看门狗";
        case ESP_RST_TASK_WDT: return "任务看门狗";
        case ESP_RST_WDT:      return "其他看门狗";
        case ESP_RST_DEEPSLEEP:return "深度睡眠唤醒";
        case ESP_RST_BROWNOUT: return "欠压复位";
        case ESP_RST_SDIO:     return "SDIO 复位";
        default:               return "未知";
    }
}

String SysInfo::formatBytes(uint64_t b) {
    char buf[24];
    if (b < 1024ULL)            snprintf(buf, sizeof(buf), "%u B", (unsigned)b);
    else if (b < 1048576ULL)    snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else if (b < 1073741824ULL) snprintf(buf, sizeof(buf), "%.2f MB", b / 1048576.0);
    else                        snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
    return String(buf);
}

uint8_t SysInfo::heapHealth() {
    uint32_t total = ESP.getHeapSize();
    uint32_t freeH = ESP.getFreeHeap();
    uint32_t maxBlk= ESP.getMaxAllocHeap();
    if (!total) return 0;
    // 综合考虑剩余量与最大连续块（碎片化会让后者明显偏小）
    float ratioFree = (float)freeH / total;
    float ratioFrag = freeH ? (float)maxBlk / freeH : 0;
    float score = (ratioFree * 0.6f + ratioFrag * 0.4f) * 100.0f;
    if (score > 100) score = 100;
    if (score < 0)   score = 0;
    return (uint8_t)score;
}

void SysInfo::toJsonStatic(JsonObject root) {
    esp_chip_info_t info;
    esp_chip_info(&info);

    JsonObject fw = root["fw"].to<JsonObject>();
    fw["name"]      = FW_NAME;
    fw["version"]   = FW_VERSION;
    fw["buildTime"] = FW_BUILD_TIME;
    fw["sdk"]       = ESP.getSdkVersion();
    fw["arduino"]   = String(ESP_ARDUINO_VERSION_MAJOR) + "." +
                      ESP_ARDUINO_VERSION_MINOR + "." + ESP_ARDUINO_VERSION_PATCH;
    fw["configVer"] = CONFIG_VERSION;

    JsonObject chip = root["chip"].to<JsonObject>();
    chip["model"]     = ESP.getChipModel();
    chip["revision"]  = ESP.getChipRevision();
    chip["cores"]     = ESP.getChipCores();
    chip["freqMHz"]   = ESP.getCpuFreqMHz();
    String feat;
    if (info.features & CHIP_FEATURE_WIFI_BGN)  feat += "WiFi b/g/n ";
    if (info.features & CHIP_FEATURE_BT)        feat += "蓝牙经典 ";
    if (info.features & CHIP_FEATURE_BLE)       feat += "BLE ";
    feat += (info.features & CHIP_FEATURE_EMB_FLASH) ? "内置Flash" : "外置Flash";
    chip["features"]  = feat;
    chip["mac"]       = macAddress();
    chip["hardware"]  = FW_HW_NAME;

    JsonObject flash = root["flash"].to<JsonObject>();
    flash["size"]      = ESP.getFlashChipSize();
    flash["sizeText"]  = formatBytes(ESP.getFlashChipSize());
    flash["speedMHz"]  = ESP.getFlashChipSpeed() / 1000000;
    flash["sketchSize"]= ESP.getSketchSize();
    flash["sketchFree"]= ESP.getFreeSketchSpace();
    flash["sketchMD5"] = ESP.getSketchMD5();

    // 运行分区 / 启动分区，用于确认 OTA 是否生效
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();
    JsonObject part = root["partition"].to<JsonObject>();
    part["running"]     = running ? running->label : "?";
    part["runningSize"] = running ? running->size : 0;
    part["boot"]        = boot ? boot->label : "?";
    part["pending"]     = (running && boot && running != boot);

    root["resetReason"] = resetReasonText();
}

void SysInfo::toJsonRuntime(JsonObject root) {
    root["uptime"]     = uptimeSeconds();
    root["uptimeText"] = uptimeString();

    float t = cpuTemperature();
    root["tempC"]      = isnan(t) ? (float)0 : t;
    root["tempValid"]  = !isnan(t);

    JsonObject heap = root["heap"].to<JsonObject>();
    heap["total"]    = ESP.getHeapSize();
    heap["free"]     = ESP.getFreeHeap();
    heap["minFree"]  = ESP.getMinFreeHeap();
    heap["maxAlloc"] = ESP.getMaxAllocHeap();
    heap["usedPct"]  = ESP.getHeapSize() ?
                       (uint8_t)(100 - (ESP.getFreeHeap() * 100ULL / ESP.getHeapSize())) : 0;
    heap["health"]   = heapHealth();

    root["freqMHz"]  = getCpuFrequencyMhz();
    root["taskCount"]= uxTaskGetNumberOfTasks();
}
