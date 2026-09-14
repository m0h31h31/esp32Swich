#include "core/Logger.h"
#include <stdarg.h>
#include <time.h>

LogEntry          Logger::_ring[LOG_RING_CAPACITY];
volatile uint16_t Logger::_head   = 0;
volatile uint16_t Logger::_count  = 0;
volatile uint32_t Logger::_nextId = 1;
LogLevel          Logger::_level  = LL_INFO;
SemaphoreHandle_t Logger::_mutex  = nullptr;
bool              Logger::_inited = false;

static const char* const kLevelNames[] = {"ERROR", "WARN", "INFO", "DEBUG"};

void Logger::begin(uint32_t baud, LogLevel level) {
    if (_inited) return;
    Serial.begin(baud);
    _mutex  = xSemaphoreCreateMutex();
    _level  = level;
    _inited = true;
    // 等待串口就绪（最多 300ms，避免上电首条日志丢失）
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 300) { delay(10); }
    Serial.println();
    Serial.printf("\n==== %s v%s 启动 ====\n", FW_NAME, FW_VERSION);
}

void Logger::setLevel(LogLevel level) { _level = level; }

const char* Logger::levelName(uint8_t level) {
    return level < 4 ? kLevelNames[level] : "?";
}

void Logger::lock() {
    if (_mutex) xSemaphoreTake(_mutex, pdMS_TO_TICKS(50));
}
void Logger::unlock() {
    if (_mutex) xSemaphoreGive(_mutex);
}

void Logger::log(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level > _level) return;

    char buf[LOG_MSG_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    lock();
    LogEntry& e = _ring[_head];
    e.id     = _nextId++;
    e.uptime = millis() / 1000;
    time_t now = time(nullptr);
    e.epoch  = (now > 1600000000) ? now : 0;   // 时间未同步则记 0
    e.level  = level;
    strncpy(e.tag, tag, sizeof(e.tag) - 1);
    e.tag[sizeof(e.tag) - 1] = '\0';
    strncpy(e.msg, buf, sizeof(e.msg) - 1);
    e.msg[sizeof(e.msg) - 1] = '\0';

    _head = (_head + 1) % LOG_RING_CAPACITY;
    if (_count < LOG_RING_CAPACITY) _count++;
    unlock();

    // 同步输出到串口
    if (_inited) {
        char ts[24];
        if (e.epoch) {
            struct tm tmv;
            localtime_r(&e.epoch, &tmv);
            strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);
        } else {
            snprintf(ts, sizeof(ts), "%8lus", (unsigned long)e.uptime);
        }
        Serial.printf("[%s][%-5s][%s] %s\n", ts, kLevelNames[level], e.tag, e.msg);
    }
}

uint32_t Logger::lastId() { return _nextId - 1; }

/// 把字符串按 JSON 规则转义后追加到 out
static void appendEscaped(String& out, const char* s) {
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)*p < 0x20) {
                    char tmp[7];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    out += tmp;
                } else {
                    out += *p;
                }
        }
    }
}

size_t Logger::toJson(uint32_t since, String& out, size_t limit) {
    out = "[";
    size_t written = 0;

    lock();
    // 从最旧的一条开始遍历
    uint16_t start = (_head + LOG_RING_CAPACITY - _count) % LOG_RING_CAPACITY;
    for (uint16_t i = 0; i < _count && written < limit; i++) {
        const LogEntry& e = _ring[(start + i) % LOG_RING_CAPACITY];
        if (e.id <= since) continue;
        if (written) out += ',';
        out += "{\"id\":";     out += e.id;
        out += ",\"up\":";     out += e.uptime;
        out += ",\"ts\":";     out += (uint32_t)e.epoch;
        out += ",\"lv\":";     out += e.level;
        out += ",\"tag\":\"";  appendEscaped(out, e.tag);
        out += "\",\"msg\":\"";appendEscaped(out, e.msg);
        out += "\"}";
        written++;
    }
    unlock();

    out += ']';
    return written;
}

void Logger::toText(String& out) {
    out = "";
    out.reserve(_count * 80);
    lock();
    uint16_t start = (_head + LOG_RING_CAPACITY - _count) % LOG_RING_CAPACITY;
    for (uint16_t i = 0; i < _count; i++) {
        const LogEntry& e = _ring[(start + i) % LOG_RING_CAPACITY];
        char line[LOG_MSG_LEN + 64];
        if (e.epoch) {
            struct tm tmv;
            localtime_r(&e.epoch, &tmv);
            char ts[24];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
            snprintf(line, sizeof(line), "[%s][%-5s][%s] %s\n",
                     ts, kLevelNames[e.level], e.tag, e.msg);
        } else {
            snprintf(line, sizeof(line), "[%8lus][%-5s][%s] %s\n",
                     (unsigned long)e.uptime, kLevelNames[e.level], e.tag, e.msg);
        }
        out += line;
    }
    unlock();
}

void Logger::clear() {
    lock();
    _head  = 0;
    _count = 0;
    unlock();
}
