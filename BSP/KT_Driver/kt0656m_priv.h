/**
 * @file    kt0656m_priv.h
 * @brief   KT0656M 驱动内部接口 —— 仅 kt0656m.c 使用，业务层不要包含
 *
 * @note    单独抽出来是为了让"总线层"和"控制层"能分文件编译，
 *          同时不把 RMW / 轮询这些实现细节暴露到 kt0656m.h 的公共 API 里。
 */
#ifndef KT0656M_PRIV_H_
#define KT0656M_PRIV_H_

#include "kt0656m.h"

/**
 * @brief  写一个 8 位寄存器（16 位寄存器地址）
 * @retval KT_OK / 负的 iic_status_t / KT_ERR_NOT_READY
 */
int kt_wr(kt0656m_dev_t *dev, uint16_t reg, uint8_t val);

/**
 * @brief  读一个 8 位寄存器
 * @retval KT_OK / 负的 iic_status_t / KT_ERR_NOT_READY
 */
int kt_rd(kt0656m_dev_t *dev, uint16_t reg, uint8_t *val);

/**
 * @brief  读-改-写：reg = (reg & ~mask) | (val & mask)
 * @note   取代原厂满篇的 regx=KT_Bus_Read(); KT_Bus_Write((regx & ~m)|v) 三行式，
 *         顺带保证读失败时不会把垃圾值写回芯片
 * @retval KT_OK / 错误码
 */
int kt_upd(kt0656m_dev_t *dev, uint16_t reg, uint8_t mask, uint8_t val);

/**
 * @brief  轮询直到 (reg & mask) == want，或超时
 * @param  timeout_ms  超时上界，超时返回 KT_ERR_TIMEOUT
 * @retval KT_OK / KT_ERR_TIMEOUT / iic 错误
 */
int kt_poll(kt0656m_dev_t *dev, uint16_t reg, uint8_t mask, uint8_t want,
            uint32_t timeout_ms);

#endif /* KT0656M_PRIV_H_ */
