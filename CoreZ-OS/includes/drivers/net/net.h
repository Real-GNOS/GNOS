#ifndef NIITAN_NET_H
#define NIITAN_NET_H

#include <stdint.h>
#include "kernel/nt_ping_reply.h"
#include "kernel/sched/sync.h"
#include "drivers/net/netif.h"

extern NETIF g_netif;

/**
 * 网络栈互斥锁。
 *
 * @remarks
 * 保护协议栈的全部共享状态：ARP 缓存、IP 待发包、ICMP ping 队列、TCP/UDP 的
 * PCB 表与收发环形缓冲、socket 表。
 * 收包在 net 内核线程的任务上下文完成（不是中断上下文），因此这里用可阻塞的
 * SCHED_LOCK 而不再关中断：持锁期间中断保持开启，畸形报文也不会拉长中断关闭窗口，
 * 线程切换由调度器按锁的等待队列处理
 *
 * @warning
 * 持锁期间不得阻塞：收发路径只做内存操作与网卡 MMIO，不调用 thread_block 与睡眠；
 * 需要睡眠的等待（sock_block、net_select）必须在持锁之外进行，否则会死锁
 */
extern struct SCHED_LOCK net_lock;

extern int net_enable;
void net_check_guards(void);
void net_init(void);
uint32_t net_now_ms(void);
int nt_icmp_send(uint32_t dst, uint16_t id, uint16_t seq);
int nt_icmp_recv(struct NET_PING_REPLY *out, int max);

#endif
