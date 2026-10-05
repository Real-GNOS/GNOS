#ifndef DRIVERS_CONSOLE_IO_H
#define DRIVERS_CONSOLE_IO_H
#include <stddef.h>
#include <stdint.h>

void io_init(uint8_t *vram, int scrnx, int scrny, uint32_t vram_bytes,
             int pitch, int bpp);
void set_text_color(int color);
uint32_t io_ansi_color(int idx);
void set_cursor(int x, int y);
int get_cursor_x(void);
int get_cursor_y(void);

void kprintf(const char *fmt, ...);

void console_putc(char c);
void console_vt_write(const char *buf, uint32_t n);

void io_clear_screen(void);

void show_char(uint8_t *vram, int pitch, int x, int y, int scrnx, int scrny,
               char c, uint32_t color, int bg);
void show_string(uint8_t *vram, int pitch, int x, int y, int scrnx, int scrny,
                 const char *s, uint32_t color, int bg);

uint8_t *io_get_vram(void);
int io_get_scrnx(void);
int io_get_scrny(void);
int io_get_pitch(void);
int io_get_bpp(void);
size_t io_get_vram_bytes(void);
void io_set_gui_active(int on);
#endif
