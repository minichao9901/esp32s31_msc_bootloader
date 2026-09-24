/*
 * usb_osal_bare.c -- CherryUSB 的裸机 OSAL
 *
 * 非线程模式（EP0 在 ISR 里处理）下 CherryUSB 实际只会用到临界区；
 * 其余线程/信号量/队列/定时器 API 给 no-op 桩，保证链接不报错。
 */
#include "usb_osal.h"
#include <stdint.h>

static inline uint32_t irq_save(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    __asm__ volatile ("csrc mstatus, %0" : : "r"(0x8u));
    return mstatus;
}

static inline void irq_restore(uint32_t f)
{
    __asm__ volatile ("csrs mstatus, %0" : : "r"(f & 0x8u));
}

/* ---- 临界区：关/开 mstatus.MIE ---- */
size_t usb_osal_enter_critical_section(void)
{
    return (size_t)irq_save();
}

void usb_osal_leave_critical_section(size_t flag)
{
    irq_restore((uint32_t)flag);
}

/* ---- 延时 ---- */
void s31_delay_ms(uint32_t ms);

void usb_osal_msleep(uint32_t delay)
{
    s31_delay_ms(delay);
}

/* ---- 堆：本配置用不到 ---- */
void *usb_osal_malloc(size_t size)
{
    (void)size;
    return 0;
}

void usb_osal_free(void *ptr)
{
    (void)ptr;
}

/* ---- 线程 / 信号量 / 互斥量 / 队列 / 定时器：no-op 桩 ---- */
usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio,
                                         usb_thread_entry_t entry, void *args)
{
    (void)name; (void)stack_size; (void)prio; (void)entry; (void)args;
    return 0;
}
void usb_osal_thread_delete(usb_osal_thread_t thread) { (void)thread; }
void usb_osal_thread_schedule_other(void) { }

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count) { (void)initial_count; return 0; }
void usb_osal_sem_delete(usb_osal_sem_t sem) { (void)sem; }
int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout) { (void)sem; (void)timeout; return 0; }
int usb_osal_sem_give(usb_osal_sem_t sem) { (void)sem; return 0; }
void usb_osal_sem_reset(usb_osal_sem_t sem) { (void)sem; }

usb_osal_mutex_t usb_osal_mutex_create(void) { return 0; }
void usb_osal_mutex_delete(usb_osal_mutex_t mutex) { (void)mutex; }
int usb_osal_mutex_take(usb_osal_mutex_t mutex) { (void)mutex; return 0; }
int usb_osal_mutex_give(usb_osal_mutex_t mutex) { (void)mutex; return 0; }

usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs) { (void)max_msgs; return 0; }
void usb_osal_mq_delete(usb_osal_mq_t mq) { (void)mq; }
int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr) { (void)mq; (void)addr; return 0; }
int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    (void)mq; (void)addr; (void)timeout;
    return 0;
}

struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms,
                                             usb_timer_handler_t handler, void *argument, bool is_period)
{
    (void)name; (void)timeout_ms; (void)handler; (void)argument; (void)is_period;
    return 0;
}
void usb_osal_timer_delete(struct usb_osal_timer *timer) { (void)timer; }
void usb_osal_timer_start(struct usb_osal_timer *timer) { (void)timer; }
void usb_osal_timer_stop(struct usb_osal_timer *timer) { (void)timer; }
