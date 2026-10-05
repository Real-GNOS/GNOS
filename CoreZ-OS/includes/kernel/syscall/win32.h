#ifndef SYSCALL_WIN32_H
#define SYSCALL_WIN32_H

#include "kernel/asm/stub.h"
#include <stdint.h>

#define WIN32_SYSCALL_BASE 0x60000u
#define WIN_THUNK_SIZE 16u
#define WIN_THUNK_SLOTS 256u
#define WIN_SHADOW 0x20u
#define WIN_STACK_BASE (WIN_SHADOW + 8u)
#define WIN_TEB_SELF 0x30u
#define WIN_TEB_STACK_BASE 0x08u
#define WIN_TEB_STACK_LIMIT 0x10u
#define WIN_RT_ARGC 0x100u
#define WIN_RT_ARGV 0x108u
#define WIN_RT_ENVP 0x110u
#define WIN_RT_FMODE 0x118u
#define WIN_RT_COMMODE 0x11Cu
#define WIN_RT_FILE 0x200u
#define WIN_RT_FILE_STRIDE 0x100u
#define WIN_FMT_MAX 256u
#define WIN_NAME_MAX 96u
#define WIN_IO_BUF 512u
#define WIN_STR_CHUNK 64u
#define WIN_STR_MAX 4096u
#define WIN_STD_INPUT 0xfffffff6u
#define WIN_STD_OUTPUT 0xfffffff5u
#define WIN_STD_ERROR 0xfffffff4u
#define WIN_PAGE_EXECUTE_READWRITE 0x40u
#define WIN_MEMORY_BASIC_SIZE 0x30u

void win32_thunk_init(uint32_t base);
uint32_t win32_lookup(const char *mod, const char *name);
uint32_t win32_resolve(const char *mod, const char *name);
int64_t win32_handler(struct ARCH_REGS *r);

uint64_t win_heap_alloc(uint32_t need, int zero);
int32_t win_user_name(char *dst, uint32_t cap, uint64_t uptr);

int64_t w32_register_class_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3);
int64_t w32_create_window_ex_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3);
int64_t w32_show_window(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
int64_t w32_update_window(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);
int64_t w32_get_message_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);
int64_t w32_peek_message_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3);
int64_t w32_translate_message(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3);
int64_t w32_dispatch_message_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3);
int64_t w32_def_window_proc_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3);
int64_t w32_post_quit_message(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3);
int64_t w32_post_message_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3);
int64_t w32_destroy_window(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3);
int64_t w32_get_dc(struct ARCH_REGS *r, uint64_t a0, uint64_t a1, uint64_t a2,
                   uint64_t a3);
int64_t w32_release_dc(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
int64_t w32_begin_paint(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
int64_t w32_end_paint(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                      uint64_t a2, uint64_t a3);
int64_t w32_get_client_rect(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3);
int64_t w32_invalidate_rect(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3);
int64_t w32_load_cursor_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);
int64_t w32_load_icon_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
int64_t w32_message_box_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);
int64_t w32_set_window_text_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3);
int64_t w32_get_system_metrics(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3);
int64_t w32_message_beep(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                         uint64_t a2, uint64_t a3);

int64_t w32_create_compatible_dc(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3);
int64_t w32_create_compatible_bitmap(struct ARCH_REGS *r, uint64_t a0,
                                     uint64_t a1, uint64_t a2, uint64_t a3);
int64_t w32_create_dib_section(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3);
int64_t w32_select_object(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);
int64_t w32_delete_object(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);
int64_t w32_delete_dc(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                      uint64_t a2, uint64_t a3);
int64_t w32_bit_blt(struct ARCH_REGS *r, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3);
int64_t w32_stretch_blt(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
int64_t w32_pat_blt(struct ARCH_REGS *r, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3);
int64_t w32_rectangle(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                      uint64_t a2, uint64_t a3);
int64_t w32_ellipse(struct ARCH_REGS *r, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3);
int64_t w32_move_to_ex(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
int64_t w32_line_to(struct ARCH_REGS *r, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3);
int64_t w32_text_out_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
int64_t w32_draw_text_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
int64_t w32_set_text_color(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3);
int64_t w32_set_bk_color(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                         uint64_t a2, uint64_t a3);
int64_t w32_set_bk_mode(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
int64_t w32_create_solid_brush(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3);
int64_t w32_create_pen(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
int64_t w32_get_stock_object(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3);
int64_t w32_fill_rect(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                      uint64_t a2, uint64_t a3);
int64_t w32_frame_rect(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
int64_t w32_set_pixel(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                      uint64_t a2, uint64_t a3);
int64_t w32_get_device_caps(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3);

#endif