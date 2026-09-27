/*---------------------------------------------------------------------------/
/  FatFs - Configuration file (GNOS kernel port)
/  Upstream default preserved beside this file as ffconf_upstream.h.  This
/  one is the kernel's: a single 512-byte-sector volume, no long file names
/  (the kernel has no code page tables wired up yet), no re-entrancy (the
/  big kernel lock already serialises filesystem code) and no RTC-derived
/  stamps.  FatFs itself is under the FatFs License -- see LICENSE.txt.
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80386	/* Revision ID */

/* Function and buffer configuration */
#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_STRFUNC	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		1
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	1
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_TRIM      0

/* Locale and namespace configuration */
#define FF_CODE_PAGE	437
#define FF_USE_LFN		0
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0
#define FF_STRF_ENCODE	3
#define FF_FS_RPATH		0

/* Volume/partition configuration */
#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_FS_EXFAT		0
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

/* System configuration */
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
#define FF_SYNC_t		int
#define FF_FS_LOCK		0
#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
#define FF_FS_NOFSINFO	0
