/* SPDX-License-Identifier: GPL-2.0 */
/*
 * calsa.h — calsa, the compat ALSA layer (/dev/snd/* over audiodev). (GPLv2)
 *
 * The frontend is deliberately one function: publish the /dev/snd nodes.
 * Which card sits behind them is audiodev's answer, not ours.
 */
#ifndef GNUCOS_CALSA_H
#define GNUCOS_CALSA_H

int calsa_vfs_register(void);

#endif
