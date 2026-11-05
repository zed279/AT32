/**
 * @file    logger.c
 * @brief   分级日志系统实现
 */
#include "logger.h"
#include "bm_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/*==============================================================================
 * 运行时状态
 *============================================================================*/
static LogLevel_t s_current_level = LOG_LEVEL_INFO;

/*==============================================================================
 * 输出后端
 *============================================================================*/
#if LOG_USE_PRINTF

/* 使用 printf 输出 (需在目标平台重定向 printf) */
static void Logger_Output(const char* str, uint16_t len)
{
    (void)len;
    printf("%s", str);
}

#else

/* 使用自定义输出函数 */
static void (*s_output_func)(const char* str, uint16_t len) = NULL;

void Logger_SetOutput(void (*output_func)(const char* str, uint16_t len))
{
    s_output_func = output_func;
}

static void Logger_Output(const char* str, uint16_t len)
{
    if (s_output_func) {
        s_output_func(str, len);
    }
}

#endif /* LOG_USE_PRINTF */

/*==============================================================================
 * 输出字符串 (供 LOG_RAW 宏使用)
 *============================================================================*/
void Logger_OutputString(const char* str)
{
    Logger_Output(str, (uint16_t)strlen(str));
}

/*==============================================================================
 * 获取级别名称
 *============================================================================*/
static const char* Logger_GetLevelName(LogLevel_t level)
{
    switch(level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_TRACE: return "TRACE";
        default:              return "NONE";
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void Logger_Init(void)
{
    s_current_level = LOG_LEVEL_INFO;

    LOG_INFO("SYS", "Logger initialized, level: %s",
             Logger_GetLevelName(s_current_level));
}

/*==============================================================================
 * 运行时设置级别
 *============================================================================*/
void Logger_SetLevel(LogLevel_t level)
{
    s_current_level = level;
    LOG_INFO("SYS", "Log level changed to: %s", Logger_GetLevelName(level));
}

LogLevel_t Logger_GetLevel(void)
{
    return s_current_level;
}

/*==============================================================================
 * 模块 TAG 过滤 (预留接口)
 *============================================================================*/
void Logger_EnableTag(const char* tag, bool enable)
{
    (void)tag;
    (void)enable;
    /* 预留: 可扩展为按 TAG 过滤的日志输出 */
}

/*==============================================================================
 * 核心日志输出
 *============================================================================*/
void Logger_Log(LogLevel_t level,
                const char* file,
                int line,
                const char* func,
                const char* tag,
                const char* fmt, ...)
{
    /* 1. 级别过滤 */
    if (level > s_current_level) {
        return;
    }

    /* 2. 组装日志 */
    char buffer[256];
    int pos = 0;

    /* 2.1 时间戳 */
    #if LOG_INCLUDE_TIME
    pos += snprintf(buffer + pos, sizeof(buffer) - pos,
                    "[%u] ", (unsigned)BM_GetTick());
    #endif

    /* 2.2 级别 */
    pos += snprintf(buffer + pos, sizeof(buffer) - pos,
                    "[%s] ", Logger_GetLevelName(level));

    /* 2.3 模块 TAG */
    if (tag) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos,
                        "[%s] ", tag);
    }

    /* 2.4 代码位置 (仅取文件名, 去除路径) */
    #if LOG_INCLUDE_LOCATION
    const char* filename = file;
    const char* last_slash = strrchr(file, '/');
    if (last_slash) {
        filename = last_slash + 1;
    }
    #ifdef _WIN32
    last_slash = strrchr(file, '\\');
    if (last_slash) {
        filename = last_slash + 1;
    }
    #endif
    pos += snprintf(buffer + pos, sizeof(buffer) - pos,
                    "%s:%d ", filename, line);
    #endif

    /* 2.5 用户消息 */
    va_list args;
    va_start(args, fmt);
    pos += vsnprintf(buffer + pos, sizeof(buffer) - pos, fmt, args);
    va_end(args);

    /* 2.6 换行 */
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "\r\n");

    /* 3. 输出 */
    Logger_Output(buffer, (uint16_t)pos);
}
