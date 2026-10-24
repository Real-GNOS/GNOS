/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ps2.c — i8042 controller setup and the auxiliary-port IRQ. (GPLv2)
 *
 * tty.c owns IRQ 1 and the keyboard stream; this file enables the second
 * port, programs the controller's configuration byte and drains IRQ 12
 * into the mouse packet decoder.  The split matches Uinxed's ps2/ps2.c.
 */
#include "ps2.h"
#include "ps2_mouse.h"
#include "idt.h"
#include "io.h"

static void ps2_wait_input_clear(void)
{
    while (inb(PS2_STATUS) & PS2_ST_INPT_BUF)
        ;
}

/* Wait for the controller to have a byte, then read it.  Reading 0x60
 * back after the 0x20 command without this wait returns stale/garbage
 * and writes that garbage back as the configuration byte. */
static uint8_t ps2_read_data(void)
{
    for (unsigned spin = 0; spin < 100000; spin++)
        if (inb(PS2_STATUS) & PS2_ST_OUT_BUF)
            return inb(PS2_DATA);
    return 0; /* controller did not answer: keep the old config intact */
}

static void ps2_write_cmd(uint8_t cmd)
{
    ps2_wait_input_clear();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data)
{
    ps2_wait_input_clear();
    outb(PS2_DATA, data);
}

/* Drain the mouse's command ACKs after a 0xF6/0xF4 write. */
static void ps2_drain_output(void)
{
    while (inb(PS2_STATUS) & PS2_ST_OUT_BUF)
        (void)inb(PS2_DATA);
}

static void ps2_enable_aux(void)
{
    ps2_write_cmd(0xA8); /* enable auxiliary port */
    ps2_wait_input_clear();

    /*
     * Read the 8042 command byte, then write it back with the two interrupt
     * enable bits set: bit0 = keyboard IRQ 1, bit1 = mouse IRQ 12.
     *
     * The previous code issued 0x60 ("write command byte") and then *read*
     * the data port, which returns whatever happens to be there (typically
     * garbage), ORed 0x02 into that and wrote it back.  The read cleared
     * bit0 in the process, so the keyboard IRQ was silently disabled for the
     * whole session -- the "login cannot accept input" symptom.  Reading the
     * command byte is a separate 0x20 command; 0x60 is write-only.
     */
    ps2_write_cmd(0x20); /* read command byte */
    uint8_t cfg = ps2_read_data();
    cfg |= 0x03; /* enable IRQ 1 (kbd) + IRQ 12 (mouse) */

    ps2_write_cmd(0x60); /* write command byte */
    ps2_write_data(cfg);

    /* Configure the aux DEVICE itself.  Bytes for the mouse must be
     * prefixed with 0xD4 on the command port -- writing them straight to
     * the data port delivers them to the keyboard instead, and the mouse
     * silently never starts reporting (IRQ12 never fires, /dev/input/
     * event1 stays empty, the X cursor cannot move). */
    ps2_write_cmd(0xD4);
    ps2_write_data(PS2_DEV_SET_DEFAULTS);
    ps2_drain_output();

    ps2_write_cmd(0xD4);
    ps2_write_data(PS2_DEV_ENABLE);
    ps2_drain_output();
}

static void ps2_mouse_irq(regs_t *r)
{
    (void)r;
    while ((inb(PS2_STATUS) & (PS2_ST_OUT_BUF | PS2_ST_AUX_DATA)) ==
           (PS2_ST_OUT_BUF | PS2_ST_AUX_DATA))
        ps2_mouse_handle_byte(ps2_read_data());
}

void ps2_init(void)
{
    ps2_enable_aux();
    irq_install(12, ps2_mouse_irq, "mouse");
}
