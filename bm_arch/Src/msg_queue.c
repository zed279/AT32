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
    uint8_t ok = 0;

    if (msg == NULL) return 0;

    /* 判满 / 写缓冲 / 推进 tail / count 自增 必须是一个原子的整体。
     * 早期实现只把 count++ 放进临界区, 于是存在这个窗口:
     *   任务 Send 写完 buffer 但还没执行 tail++ 时被中断打断,
     *   中断里的 Send 读到同一个 tail, 写到同一个槽位 —— 后写覆盖前写,
     *   两边各自 count++, 结果是队列里一条数据重复、另一条真数据丢失。
     * 当前只有 EXINT0 一个生产者所以没暴露, 一旦编码器 EXTI / 射频事件
     * 也开始投递就会踩中。整段关中断的代价只有一次 12 字节 memcpy。 */
    BM_EnterCritical();

    if (s_queue.count < MSG_QUEUE_SIZE) {
        memcpy((void*)&s_queue.buffer[s_queue.tail], msg, sizeof(Message_t));
        s_queue.tail = (s_queue.tail + 1) & (MSG_QUEUE_SIZE - 1);
        s_queue.count++;
        ok = 1;
    }
    /* else: 队列满, 静默丢弃 (返回 0 由调用者决定是否重试) */

    BM_ExitCritical();

    return ok;
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
