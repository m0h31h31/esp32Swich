/**
 * @file    Logger.h
 * @brief   运行日志模块：串口输出 + 内存环形缓冲（供网页查询）
 *
 * 设计要点：
 *  - 使用固定大小的静态环形缓冲，避免 String 频繁分配造成堆碎片；
 *  - 每条日志带自增 id，网页端用 since=id 做增量拉取，省流量；
 *  - 带互斥锁，可从任意任务安全调用。
 */
#pragma once

#include <Arduino.h>
#include "app_config.h"

/// 日志级别（数值越小越严重）
enum LogLevel : uint8_t {
    LL_ERROR = 0,
    LL_WARN  = 1,
    LL_INFO  = 2,
    LL_DEBUG = 3,
};

/// 单条日志记录
struct LogEntry {
    uint32_t id;                 ///< 自增序号
    uint32_t uptime;             ///< 记录时刻的开机秒数
    time_t   epoch;              ///< 记录时刻的 UTC 时间戳（未同步时为 0）
    uint8_t  level;              ///< LogLevel
    char     tag[10];            ///< 模块标签
    char     msg[LOG_MSG_LEN];   ///< 日志正文
};

class Logger {
public:
    /// 初始化（会启动串口）
    static void begin(uint32_t baud, LogLevel level = LL_INFO);

    static void     setLevel(LogLevel level);
    static LogLevel getLevel() { return _level; }

    /// 写入一条日志（printf 风格）
    static void log(LogLevel level, const char* tag, const char* fmt, ...);

    /// 当前最新日志 id（0 表示尚无日志）
    static uint32_t lastId();

    /**
     * 导出 id 大于 since 的日志为 JSON 数组字符串
     * @param since  客户端已拥有的最大 id
     * @param out    输出缓冲
     * @param limit  本次最多返回条数
     * @return 实际返回条数
     */
    static size_t toJson(uint32_t since, String& out, size_t limit = LOG_RING_CAPACITY);

    /// 导出为纯文本（用于下载 / 串口打印）
    static void toText(String& out);

    /// 清空日志缓冲
    static void clear();

    static const char* levelName(uint8_t level);

private:
    static LogEntry          _ring[LOG_RING_CAPACITY];
    static volatile uint16_t _head;      ///< 下一个写入位置
    static volatile uint16_t _count;     ///< 已使用条数
    static volatile uint32_t _nextId;
    static LogLevel          _level;
    static SemaphoreHandle_t _mutex;
    static bool              _inited;

    static void lock();
    static void unlock();
};

// ---- 便捷宏：自动带模块标签 ----
#define LOG_E(tag, ...) Logger::log(LL_ERROR, tag, __VA_ARGS__)
#define LOG_W(tag, ...) Logger::log(LL_WARN,  tag, __VA_ARGS__)
#define LOG_I(tag, ...) Logger::log(LL_INFO,  tag, __VA_ARGS__)
#define LOG_D(tag, ...) Logger::log(LL_DEBUG, tag, __VA_ARGS__)
