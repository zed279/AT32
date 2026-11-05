/**
 * @file    logger_cfg.h
 * @brief   日志系统配置
 *
 * @details 用户可在此文件中裁剪日志级别和输出格式。
 *          修改后无需改动 logger.c 源码。
 */
#ifndef __LOGGER_CFG_H
#define __LOGGER_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 各级别日志编译开关 (1=编译, 0=完全移除, 节省Flash)
 *
 * 【本板全部置 0】板上没有引出调试串口，且 PA9 已被组2 KT 芯片的 SCL 占用
 * （AT32 BSP 的打印串口固定是 USART1 = PA9），日志无处可去。
 * 全部置 0 后 LOG_xxx 宏展开为 ((void)0)，连参数表达式都不会求值，
 * 既省 Flash 也不会触发任何 USART 访问。
 *
 * 将来要恢复打印：换一个不冲突的 USART 重定向 printf，
 * 再把需要的级别打开并把 LOG_USE_PRINTF 置 1。
 *============================================================================*/
#define LOG_ENABLE_ERROR   0
#define LOG_ENABLE_WARN    0
#define LOG_ENABLE_INFO    0
#define LOG_ENABLE_DEBUG   0    /* 调试阶段开启, 量产可关 */
#define LOG_ENABLE_TRACE   0    /* 跟踪级别默认关闭 */

/*==============================================================================
 * 输出方式
 *   1 = 使用 printf (需在目标平台重定向 printf, 如通过 UART)
 *   0 = 使用自定义输出函数 (调用 Logger_SetOutput 注册)
 *============================================================================*/
#define LOG_USE_PRINTF     0

/*==============================================================================
 * 日志前缀格式
 *============================================================================*/
#define LOG_INCLUDE_TIME     1    /* 1=包含 SysTick 时间戳   */
#define LOG_INCLUDE_LOCATION 1    /* 1=包含 文件名:行号      */

#ifdef __cplusplus
}
#endif
#endif /* __LOGGER_CFG_H */
