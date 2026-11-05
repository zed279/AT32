/**
 * @file    msg_queue.h
 * @brief   环形消息队列 (ISR 安全)
 *
 * @details 基于环形缓冲区的消息队列，用于任务间异步通信。
 *          - 队列大小必须为 2 的幂 (使用位掩码代替取模, 提升效率)
 *          - Send/Recv 操作使用临界区保护 count 变量, 可安全在中断中调用
 *          - 纯软件实现, 不依赖任何 MCU 硬件
 */
#ifndef __MSG_QUEUE_H
#define __MSG_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*==============================================================================
 * 配置
 *============================================================================*/

/* 队列深度, 必须为 2 的幂 */
#ifndef MSG_QUEUE_SIZE
#define MSG_QUEUE_SIZE     32
#endif

/* 每条消息携带数据的最大字节数 */
#ifndef MSG_MAX_DATA_LEN
#define MSG_MAX_DATA_LEN   8
#endif

/* 编译期检查: MSG_QUEUE_SIZE 必须是 2 的幂 */
#if ((MSG_QUEUE_SIZE == 0) || (MSG_QUEUE_SIZE & (MSG_QUEUE_SIZE - 1)))
#error "MSG_QUEUE_SIZE must be a power of 2."
#endif

/*==============================================================================
 * 消息类型 (用户根据应用自定义)
 *============================================================================*/
typedef enum {
    MSG_FREQ_CHANGED = 0,     /* 示例: 频率变化       */
    MSG_SEEK_START,           /* 示例: 开始搜台       */
    MSG_SEEK_DONE,            /* 示例: 搜台完成       */
    MSG_RSSI_UPDATE,          /* 示例: 信号更新       */
    MSG_STEREO_CHANGED,       /* 示例: 立体声变化     */
    MSG_MUTE,                 /* 示例: 静音           */
    MSG_UNMUTE,               /* 示例: 取消静音       */
    MSG_BUTTON_PRESSED,       /* 应用: 用户按键按下 (EXINT0 投递) */
    /* ... 在此添加更多消息类型 ... */
} MsgType_t;

/*==============================================================================
 * 消息结构
 *============================================================================*/
typedef struct {
    MsgType_t type;                      /* 消息类型     */
    uint8_t   data_len;                  /* 数据长度     */
    uint8_t   data[MSG_MAX_DATA_LEN];    /* 负载数据     */
} Message_t;

/*==============================================================================
 * API
 *============================================================================*/
void     MsgQueue_Init(void);
void     MsgQueue_Clear(void);

uint8_t  MsgQueue_Send(Message_t *msg);
uint8_t  MsgQueue_Recv(Message_t *msg);
uint8_t  MsgQueue_Peek(Message_t *msg);

uint8_t  MsgQueue_IsEmpty(void);
uint8_t  MsgQueue_IsFull(void);
uint16_t MsgQueue_GetCount(void);

/*==============================================================================
 * 快捷宏 — 发送无数据消息
 *============================================================================*/
#define MsgQueue_SendSimple(msgType)              \
    do                                            \
    {                                             \
        Message_t _msg =                          \
        {                                         \
            .type = (msgType),                    \
            .data_len = 0                         \
        };                                        \
        MsgQueue_Send(&_msg);                     \
    } while (0)

/*==============================================================================
 * 快捷宏 — 发送携带数据的消息
 *============================================================================*/
#define MsgQueue_SendWithData(msgType, dat, len)      \
    do                                                \
    {                                                 \
        uint8_t _len = (uint8_t)(len);                \
                                                      \
        if (_len > MSG_MAX_DATA_LEN)                  \
            _len = MSG_MAX_DATA_LEN;                  \
                                                      \
        Message_t _msg;                               \
        _msg.type = (msgType);                        \
        _msg.data_len = _len;                         \
                                                      \
        if ((_len > 0U) && ((dat) != NULL))           \
        {                                             \
            memcpy(_msg.data, (dat), _len);           \
        }                                             \
                                                      \
        MsgQueue_Send(&_msg);                         \
    } while (0)

#ifdef __cplusplus
}
#endif
#endif /* __MSG_QUEUE_H */
