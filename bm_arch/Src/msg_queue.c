/**
 * @file    msg_queue.c
 * @brief   环形消息队列实现
 */
#include "msg_queue.h"
#include "bm_hal.h"

/*==============================================================================
 * 环形缓冲区内部结构
 *============================================================================*/
typedef struct {
    Message_t       buffer[MSG_QUEUE_SIZE];
    volatile uint16_t head;      /* 读指针 (消费者) */
    volatile uint16_t tail;      /* 写指针 (生产者) */
    volatile uint16_t count;     /* 当前消息数量   */
} RingBuffer_t;

static RingBuffer_t s_queue;

/*==============================================================================
 * 初始化队列
 *============================================================================*/
void MsgQueue_Init(void)
{
    memset((void*)&s_queue, 0, sizeof(s_queue));
}

/*==============================================================================
 * 清空队列
 *============================================================================*/
void MsgQueue_Clear(void)
{
    BM_EnterCritical();
    s_queue.head  = 0;
    s_queue.tail  = 0;
    s_queue.count = 0;
    BM_ExitCritical();
}

/*==============================================================================
 * 发送消息 (入队) — 可在 ISR 中安全调用
 *============================================================================*/
uint8_t MsgQueue_Send(Message_t *msg)
{
    if (msg == NULL) return 0;

    /* 检查是否已满 */
    if (s_queue.count >= MSG_QUEUE_SIZE) {
        return 0;   /* 队列满, 丢弃消息 */
    }

    /* 拷贝消息到缓冲区 */
    memcpy((void*)&s_queue.buffer[s_queue.tail], msg, sizeof(Message_t));

    /* 推进写指针 (位掩码代替取模, 要求 MSG_QUEUE_SIZE 为 2 的幂) */
    s_queue.tail = (s_queue.tail + 1) & (MSG_QUEUE_SIZE - 1);

    /* 临界区保护 count 自增 (可能在中断中调用) */
    BM_EnterCritical();
    s_queue.count++;
    BM_ExitCritical();

    return 1;
}

/*==============================================================================
 * 接收消息 (出队)
 *============================================================================*/
uint8_t MsgQueue_Recv(Message_t *msg)
{
    if (msg == NULL) return 0;

    /* 检查是否为空 */
    if (s_queue.count == 0) {
        return 0;   /* 队列空 */
    }

    /* 拷贝消息 */
    memcpy(msg, (void*)&s_queue.buffer[s_queue.head], sizeof(Message_t));

    /* 推进读指针 */
    s_queue.head = (s_queue.head + 1) & (MSG_QUEUE_SIZE - 1);

    /* 临界区保护 count 自减 */
    BM_EnterCritical();
    s_queue.count--;
    BM_ExitCritical();

    return 1;
}

/*==============================================================================
 * 偷看消息 (不出队)
 *============================================================================*/
uint8_t MsgQueue_Peek(Message_t *msg)
{
    if (msg == NULL) return 0;

    if (s_queue.count == 0) {
        return 0;
    }

    memcpy(msg, (void*)&s_queue.buffer[s_queue.head], sizeof(Message_t));
    return 1;
}

/*==============================================================================
 * 查询队列状态
 *============================================================================*/
uint8_t MsgQueue_IsEmpty(void)
{
    return (s_queue.count == 0);
}

uint8_t MsgQueue_IsFull(void)
{
    return (s_queue.count >= MSG_QUEUE_SIZE);
}

uint16_t MsgQueue_GetCount(void)
{
    return s_queue.count;
}
