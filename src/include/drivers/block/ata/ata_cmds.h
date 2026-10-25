/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ata_cmds.h — ATA/ATAPI command codes and register definitions. (GPLv2)
 *
 * Shared by the legacy PATA driver and the ATAPI packet layer.  The layout
 * follows Uinxed's include/drivers/block/ata/ata_cmds.h: register offsets,
 * status bits and the command bytes live in one place so the two halves of
 * the driver cannot drift apart.
 */
#ifndef GNOS_ATA_CMDS_H
#define GNOS_ATA_CMDS_H

/* ---- command-block register offsets ---------------------------------- */
#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1 /* read  */
#define ATA_REG_FEATURES 1 /* write */
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0     3
#define ATA_REG_LBA1     4
#define ATA_REG_LBA2     5
#define ATA_REG_DRIVE    6
#define ATA_REG_STATUS   7 /* read  */
#define ATA_REG_COMMAND  7 /* write */

/* ---- status bits ------------------------------------------------------ */
#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_RDY  0x40
#define ATA_SR_BSY  0x80

/* Device control register: nIEN stops the drive asserting its IRQ line,
 * SRST is the software reset bit. */
#define ATA_DCR_NIEN 0x02
#define ATA_DCR_SRST 0x04

/* ---- ATA commands ----------------------------------------------------- */
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_READ_PIO_EXT    0x24
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_WRITE_PIO_EXT   0x34
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_CACHE_FLUSH     0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0

/* ---- IDENTIFY DEVICE word offsets --------------------------------------
 * Shared by the PATA and SATA halves: both parse the same 256-word answer,
 * so the field positions live here and not in each driver. */
#define ATA_IDENT_DEVICETYPE    0
#define ATA_IDENT_CYLINDERS     2
#define ATA_IDENT_HEADS         6
#define ATA_IDENT_SECTORS       12
#define ATA_IDENT_SERIAL        20
#define ATA_IDENT_MODEL         54
#define ATA_IDENT_MAX_LBA       120
#define ATA_IDENT_CAPABILITIES  98
#define ATA_IDENT_FIELDVALID    106
#define ATA_IDENT_MAX_LBA_EXT   200
#define ATA_IDENT_COMMANDSETS   164

/* ---- SCSI/ATAPI packet opcodes ---------------------------------------- */
#define GPCMD_TEST_UNIT_READY  0x00
#define GPCMD_REQUEST_SENSE    0x03
#define GPCMD_INQUIRY          0x12
#define GPCMD_START_STOP_UNIT  0x1B
#define GPCMD_MODE_SENSE       0x1A
#define GPCMD_READ_CAPACITY    0x25
#define GPCMD_READ_10          0x28
#define GPCMD_WRITE_10         0x2A
#define GPCMD_GET_EVENT_STATUS 0x4A
#define GPCMD_MODE_SENSE_10    0x5A
#define GPCMD_READ_12          0xA8
#define GPCMD_WRITE_12         0xAA
#define GPCMD_READ_CD          0xBE
#define SCSI_SENSE_BUFFER_SIZE 18

/*
 * How many status reads to spend waiting.  Each `inb` on a legacy ISA port
 * costs on the order of a microsecond, so this is seconds of patience --
 * far more than a spin-up needs, and still bounded.
 */
#define ATA_WAIT_SPINS 2000000

#endif /* GNOS_ATA_CMDS_H */
