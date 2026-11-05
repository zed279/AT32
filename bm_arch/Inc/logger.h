/**
 * @file    logger.h
 * @brief   分级日志系统
 *
 * @details 支持运行时动态调整日志级别 (NONE/ERROR/WARN/INFO/DEBUG/TRACE)。
 *          通过 logger_cfg.h 可在编译期裁剪各级别, 节省 Flash 空间。
 *          输出方式可选 printf (需平台重定向) 或自定义输出函数。
 *
 * @note    使用示例:
 *            Logger_Init();
 *            Logger_SetLevel(LOG_LEVEL_DEBUG);
 *            LOG_INFO("APP", "value = %d", 42);
 *            LOG_RAW("直接打印, 无前缀\r\n");
 */
#ifndef __LOGGER_H
#define __LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include "logger_cfg.h"

/*==============================================================================
 * 日志级别 (值越大, 输出越详细)
 *============================================================================*/
typedef enum {
    LOG_LEVEL_NONE = 0,     /* 关闭所有日志 */
    LOG_LEVEL_ERROR,        /* 仅错误       */
    LOG_LEVEL_WARN,         /* 警告 + 错误  */
    LOG_LEVEL_INFO,         /* 信息及以上   */
    LOG_LEVEL_DEBUG,        /* 调试及以上   */
    LOG_LEVEL_TRACE,        /* 全部         */
} LogLevel_t;

/*==============================================================================
 * 运行时控制
 *============================================================================*/
void Logger_Init(void);
void Logger_SetLevel(LogLevel_t level);
LogLevel_t Logger_GetLevel(void);
void Logger_EnableTag(const char* tag, bool enable);

/*==============================================================================
 * 核心日志函数 (通常不直接调用, 使用下方宏)
 *============================================================================*/
void Logger_Log(LogLevel_t level,
                const char* file,
                int line,
                const char* func,
                const char* tag,
                const char* fmt, ...);

/*==============================================================================
 * 自定义输出注册 (LOG_USE_PRINTF = 0 时使用)
 *============================================================================*/
void Logger_SetOutput(void (*output_func)(const char* str, uint16_t len));
void Logger_OutputString(const char* str);

/*==============================================================================
 * 日志宏 — 编译期裁剪
 *============================================================================*/

#if LOG_ENABLE_ERROR
#define LOG_ERROR(tag, fmt, ...) \
    Logger_Log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(tag, fmt, ...) ((void)0)
#endif

#if LOG_ENABLE_WARN
#define LOG_WARN(tag, fmt, ...) \
    Logger_Log(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(tag, fmt, ...) ((void)0)
#endif

#if LOG_ENABLE_INFO
#define LOG_INFO(tag, fmt, ...) \
    Logger_Log(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(tag, fmt, ...) ((void)0)
#endif

#if LOG_ENABLE_DEBUG
#define LOG_DEBUG(tag, fmt, ...) \
    Logger_Log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(tag, fmt, ...) ((void)0)
#endif

#if LOG_ENABLE_TRACE
#define LOG_TRACE(tag, fmt, ...) \
    Logger_Log(LOG_LEVEL_TRACE, __FILE__, __LINE__, __FUNCTION__, tag, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(tag, fmt, ...) ((void)0)
#endif

/*==============================================================================
 * 原始打印宏 — 不加前缀, 直接输出
 *============================================================================*/
#define LOG_RAW(fmt, ...) \
    do { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        Logger_OutputString(_buf); \
    } while(0)

#ifdef __cplusplus
}
#endif
#endif /* __LOGGER_H */
