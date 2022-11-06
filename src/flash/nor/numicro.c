// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2011 by James K. Larson                                 *
 *   jlarson@pacifier.com                                                  *
 *                                                                         *
 *   Copyright (C) 2013 Cosmin Gorgovan                                    *
 *   cosmin [at] linux-geek [dot] org                                      *
 *                                                                         *
 *   Copyright (C) 2014 Pawel Si                                           *
 *   stawel+openocd@gmail.com                                              *
 *                                                                         *
 *   Copyright (C) 2015 Nemui Trinomius                                    *
 *   nemuisan_kawausogasuki@live.jp                                        *
 *                                                                         *
 *   Copyright (C) 2017 Zale Yu                                            *
 *   CYYU@nuvoton.com                                                      *
 *                                                                         *
 *   Copyright (C) 2022 Jian-Hong Pan                                      *
 *   chienhung.pan@gmail.com                                               *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>
#include <target/cortex_m.h>

/* Nuvoton NuMicro register locations */
#define NUMICRO_SYS_BASE        0x50000000
#define NUMICRO_SYS_WRPROT      0x50000100
#define NUMICRO_SYS_IPRSTC1     0x50000008

#define NUMICRO_SYSCLK_BASE     0x50000200
#define NUMICRO_SYSCLK_PWRCON   0x50000200
#define NUMICRO_SYSCLK_CLKSEL0  0x50000210
#define NUMICRO_SYSCLK_CLKDIV   0x50000218
#define NUMICRO_SYSCLK_AHBCLK   0x50000204

#define NUMICRO_FLASH_BASE      0x5000C000
#define NUMICRO_FLASH_ISPCON    0x5000C000
#define NUMICRO_FLASH_ISPADR    0x5000C004
#define NUMICRO_FLASH_ISPDAT    0x5000C008
#define NUMICRO_FLASH_ISPCMD    0x5000C00C
#define NUMICRO_FLASH_ISPTRG    0x5000C010
#define NUMICRO_FLASH_CHEAT	  0x5000C01C	/* Undocumented isp register(may be cheat register) */

#define NUMICRO_SCS_BASE        0xE000E000
#define NUMICRO_SCS_AIRCR       0xE000ED0C
#define NUMICRO_SCS_DHCSR       0xE000EDF0
#define NUMICRO_SCS_DEMCR       0xE000EDFC

#define NUMICRO_APROM_BASE      0x00000000
#define NUMICRO_DATA_BASE       0x0001F000
#define NUMICRO_LDROM_BASE      0x00100000
#define NUMICRO_CONFIG_BASE     0x00300000

#define NUMICRO_CONFIG0         0x5000C000
#define NUMICRO_CONFIG1         0x5000C004

/* Command register bits */
#define PWRCON_OSC22M         (1 << 2)
#define PWRCON_XTL12M         (1 << 0)

#define IPRSTC1_CPU_RST       (1 << 1)
#define IPRSTC1_CHIP_RST      (1 << 0)

#define AHBCLK_ISP_EN         (1 << 2)
#define AHBCLK_SRAM_EN        (1 << 4)
#define AHBCLK_TICK_EN        (1 << 5)

#define ISPCON_ISPEN          (1 << 0)
#define ISPCON_BS_AP          (0 << 1)
#define ISPCON_BS_LP          (1 << 1)
#define ISPCON_BS_MASK        (1 << 1)
#define ISPCON_APUEN          (1 << 3)
#define ISPCON_CFGUEN         (1 << 4)
#define ISPCON_LDUEN          (1 << 5)
#define ISPCON_ISPFF          (1 << 6)

#define CONFIG0_LOCK_MASK	  (1 << 1)

/* isp commands */
#define ISPCMD_READ           0x00
#define ISPCMD_WRITE          0x21
#define ISPCMD_ERASE          0x22
#define ISPCMD_CHIPERASE      0x26   /* Undocumented isp "Chip-Erase" command */
#define ISPCMD_READ_CID       0x0B
#define ISPCMD_READ_DID       0x0C
#define ISPCMD_READ_UID       0x04
#define ISPCMD_VECMAP         0x2E
#define ISPTRG_ISPGO          (1 << 0)

/* access unlock keys */
#define REG_KEY1              0x59
#define REG_KEY2              0x16
#define REG_KEY3              0x88
#define REG_LOCK              0x00

/* flash pagesizes */
#define NUMICRO_PAGESIZE        512
/* flash MAX banks */
#define NUMICRO_MAX_FLASH_BANKS 4

/* flash bank structs */
struct numicro_flash_bank_type {
	uint32_t base;
	uint32_t size;
};

/* part structs */
struct numicro_cpu_type {
	char *partname;
	uint32_t partid;
	unsigned int n_banks;
	struct numicro_flash_bank_type bank[NUMICRO_MAX_FLASH_BANKS];
};

/* If DataFlash size equals zero, it means the actual size depends on config settings. */
#define NUMICRO_BANKS_GENERAL(aprom_size, data_size, ldrom_size, config_size) \
	.n_banks = 4, \
	{{NUMICRO_APROM_BASE,  (aprom_size)}, \
	 {NUMICRO_DATA_BASE,   (data_size)}, \
	 {NUMICRO_LDROM_BASE,  (ldrom_size)}, \
	 {NUMICRO_CONFIG_BASE, (config_size)}}

static const struct numicro_cpu_type numicro_parts[] = {
	/*PART NO*/     /*PART ID*/ /*Banks*/
	/* M051AN */
	{"M052LAN",  0x00005200, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M054LAN",  0x00005400, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058LAN",  0x00005800, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M0516LAN", 0x00005A00, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M052ZAN",  0x00005203, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M054ZAN",  0x00005403, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058ZAN",  0x00005803, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M0516ZAN", 0x00005A03, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},

	/* M051BN */
	{"M052LBN",  0x10005200, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M054LBN",  0x10005400, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058LBN",  0x10005800, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M0516LBN", 0x10005A00, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M052ZBN",  0x10005203, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M054ZBN",  0x10005403, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058ZBN",  0x10005803, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M0516ZBN", 0x10005A03, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},

	/* M051DN */
	{"M0516LDN", 0x20005A00, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M0516ZDN", 0x20005A03, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M052LDN",  0x20005200, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M052ZDN",  0x20005203, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M054LDN",  0x20005400, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M054ZDN",  0x20005403, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058LDN",  0x20005800, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058ZDN",  0x20005803, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},

	/* M051DE */
	{"M0516LDE", 0x30005A00, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M0516ZDE", 0x30005A03, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M052LDE",  0x30005200, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M052ZDE",  0x30005203, NUMICRO_BANKS_GENERAL(8 * 1024,  4 * 1024, 4 * 1024, 4)},
	{"M054LDE",  0x30005400, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M054ZDE",  0x30005403, NUMICRO_BANKS_GENERAL(16 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058LDE",  0x30005800, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"M058ZDE",  0x30005803, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},

	/* M0518 */
	{"M0518LC2AE", 0x10051803, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"M0518LD2AE", 0x10051800, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"M0518SC2AE", 0x10051813, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"M0518SD2AE", 0x10051810, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* M0519 */
	{"M0519LD3AE", 0x00051902, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"M0519LE3AE", 0x00051900, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"M0519SD3AE", 0x00051922, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"M0519SE3AE", 0x00051920, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"M0519VE3AE", 0x00051930, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},

	/* M058S */
	{"M058SFAN", 0x00005818, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"M058SLAN", 0x00005810, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"M058SSAN", 0x00005816, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"M058SZAN", 0x00005813, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* MINI51AN */
	{"MINI51LAN", 0x00205100, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI51TAN", 0x00205104, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI51ZAN", 0x00205103, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52LAN", 0x00205200, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52TAN", 0x00205204, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52ZAN", 0x00205203, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI54LAN", 0x00205400, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},
	{"MINI54TAN", 0x00205404, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},
	{"MINI54ZAN", 0x00205403, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},

	/* MINI51DE */
	{"MINI51FDE", 0x20205105, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI51LDE", 0x20205100, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI51TDE", 0x20205104, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI51ZDE", 0x20205103, NUMICRO_BANKS_GENERAL(4 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52FDE", 0x20205205, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52LDE", 0x20205200, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52TDE", 0x20205204, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI52ZDE", 0x20205203, NUMICRO_BANKS_GENERAL(8 * 1024,  0 * 1024, 2 * 1024, 8)},
	{"MINI54FDE", 0x20205405, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},
	{"MINI54LDE", 0x20205400, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},
	{"MINI54TDE", 0x20205404, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},
	{"MINI54ZDE", 0x20205403, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},

	/* MINI55 */
	{"MINI55LDE", 0x00505500, NUMICRO_BANKS_GENERAL(35 * 512, 0 * 1024, 2 * 1024, 8)},
	{"MINI55ZDE", 0x00505503, NUMICRO_BANKS_GENERAL(35 * 512, 0 * 1024, 2 * 1024, 8)},

	/* MINI58 */
	{"MINI58FDE", 0x00A05805, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 5 * 512, 8)},
	{"MINI58LDE", 0x00A05800, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 5 * 512, 8)},
	{"MINI58TDE", 0x00A05804, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 5 * 512, 8)},
	{"MINI58ZDE", 0x00A05803, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 5 * 512, 8)},

	/* NANO100AN */
	{"NANO100LC2AN", 0x00110025, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100LD2AN", 0x00110019, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100LD3AN", 0x00110018, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100SC2AN", 0x00110023, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100SD2AN", 0x00110016, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100SD3AN", 0x00110015, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100VD2AN", 0x00110013, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100VD3AN", 0x00110012, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100ZC2AN", 0x00110029, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100ZD2AN", 0x00110028, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100ZD3AN", 0x00110027, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120LC2AN", 0x00112025, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120LD2AN", 0x00112019, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120LD3AN", 0x00112018, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120SC2AN", 0x00112023, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120SD2AN", 0x00112016, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120SD3AN", 0x00112015, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120VD2AN", 0x00112013, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120VD3AN", 0x00112012, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120ZC2AN", 0x00112029, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120ZD2AN", 0x00112028, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120ZD3AN", 0x00112027, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NANO100BN */
	{"NANO100KC2BN", 0x00110040, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100KD2BN", 0x00110039, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100KD3BN", 0x00110038, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100KE3BN", 0x00110030, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100LC2BN", 0x00110043, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100LD2BN", 0x0011003F, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100LD3BN", 0x0011003E, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100LE3BN", 0x00110036, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100ND2BN", 0x00110046, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100ND3BN", 0x00110045, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100NE3BN", 0x00110044, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO100SC2BN", 0x00110042, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100SD2BN", 0x0011003D, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100SD3BN", 0x0011003C, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO100SE3BN", 0x00110034, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO110KC2BN", 0x00111040, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110KD2BN", 0x00111039, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110KD3BN", 0x00111038, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110KE3BN", 0x00111030, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO110RC2BN", 0x00111043, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110RD2BN", 0x00111044, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110RD3BN", 0x00111045, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110SC2BN", 0x00111042, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110SD2BN", 0x0011103D, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110SD3BN", 0x0011103C, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO110SE3BN", 0x00111034, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120KC2BN", 0x00112040, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120KD2BN", 0x00112039, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120KD3BN", 0x00112038, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120KE3BN", 0x00112030, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120LC2BN", 0x00112043, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120LD2BN", 0x0011203F, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120LD3BN", 0x0011203E, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120LE3BN", 0x00112036, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO120SC2BN", 0x00112042, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120SD2BN", 0x0011203D, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120SD3BN", 0x0011203C, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO120SE3BN", 0x00112034, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO130KC2BN", 0x00113040, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO130KD2BN", 0x00113039, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO130KD3BN", 0x00113038, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO130KE3BN", 0x00113030, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO130SC2BN", 0x00113042, NUMICRO_BANKS_GENERAL(32 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO130SD2BN", 0x0011303D, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO130SD3BN", 0x0011303C, NUMICRO_BANKS_GENERAL(64 * 1024,  0 * 1024, 4 * 1024, 8)},
	{"NANO130SE3BN", 0x00113034, NUMICRO_BANKS_GENERAL(123 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NANO103 */
	{"NANO103SD3AE", 0x00110301, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO103LD3AE", 0x00110304, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO103ZD3AE", 0x00110307, NUMICRO_BANKS_GENERAL(64 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NANO112AN */
	{"NANO102LB1AN", 0x00110206, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO102LC2AN", 0x00110208, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO102SC2AN", 0x00110212, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO102ZB1AN", 0x00110202, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO102ZC2AN", 0x00110204, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112LB1AN", 0x00111202, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112LC2AN", 0x00111204, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112RB1AN", 0x00111210, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112RC2AN", 0x00111212, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112SB1AN", 0x00111206, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112SC2AN", 0x00111208, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NANO112VC2AN", 0x00111216, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NUC029AN */
	{"NUC029LAN", 0x00295A00, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 4)},
	{"NUC029TAN", 0x00295804, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 4)},

	/* NUC029AE */
	{"NUC029FAE", 0x00295415, NUMICRO_BANKS_GENERAL(16 * 1024, 0 * 1024, 2 * 1024, 8)},

	/* NUC100AN */
	{"NUC100LD3AN", 0x00010003, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100LE3AN", 0x00010000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC100RD3AN", 0x00010012, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100RE3AN", 0x00010009, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC100VD2AN", 0x00010022, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100VD3AN", 0x00010021, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100VE3AN", 0x00100018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC120LD3AN", 0x00012003, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120LE3AN", 0x00120000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC120RD3AN", 0x00012012, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120RE3AN", 0x00012009, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC120VD2AN", 0x00012022, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120VD3AN", 0x00012021, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120VE3AN", 0x00012018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NUC100BN */
	{"NUC100LC1BN", 0x10010008, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC100LD1BN", 0x10010005, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC100LD2BN", 0x10010004, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC100RC1BN", 0x10010017, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC100RD1BN", 0x10010014, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC100RD2BN", 0x10010013, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC120LC1BN", 0x10012008, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC120LD1BN", 0x10012005, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC120LD2BN", 0x10012004, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC120RC1BN", 0x10012017, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC120RD1BN", 0x10012014, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC120RD2BN", 0x10012013, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* NUC100CN */
	{"NUC130LC1CN", 0x20013008, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC130LD2CN", 0x20013004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC130LE3CN", 0x20013000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC130RC1CN", 0x20013017, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC130RD2CN", 0x20013013, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC130RE3CN", 0x20013009, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC130VE3CN", 0x20013018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC140LC1CN", 0x20014008, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC140LD2CN", 0x20014004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC140LE3CN", 0x20014000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC140RC1CN", 0x20014017, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC140RD2CN", 0x20014013, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC140RE3CN", 0x20014009, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC140VE3CN", 0x20014018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NUC100DN */
	{"NUC100LC1DN", 0x30010008, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100LD1DN", 0x30010005, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100LD2DN", 0x30010004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100LD3DN", 0x30010003, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100LE3DN", 0x30010000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC100RC1DN", 0x30010017, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100RD1DN", 0x30010014, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100RD2DN", 0x30010013, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100RD3DN", 0x30010012, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100RE3DN", 0x30010009, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC100VD2DN", 0x30010022, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100VD3DN", 0x30010021, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC100VE3DN", 0x30010018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC120LC1DN", 0x30012008, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120LD1DN", 0x30012005, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120LD2DN", 0x30012004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120LD3DN", 0x30012003, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120LE3DN", 0x30012000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC120RC1DN", 0x30012035, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120RD1DN", 0x30012032, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120RD2DN", 0x30012031, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120RD3DN", 0x30012030, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120RE3DN", 0x30012027, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC120VD2DN", 0x30012022, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120VD3DN", 0x30012021, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC120VE3DN", 0x30012018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NUC121 */
	{"NUC121SC2AE", 0x00012105, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 9 * 512, 8)},
	{"NUC121LC2AE", 0x00012125, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 9 * 512, 8)},
	{"NUC121ZC2AE", 0x00012145, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 9 * 512, 8)},
	{"NUC125SC2AE", 0x00012505, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 9 * 512, 8)},
	{"NUC125LC2AE", 0x00012525, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 9 * 512, 8)},
	{"NUC125ZC2AE", 0x00012545, NUMICRO_BANKS_GENERAL(32 * 1024, 0 * 1024, 9 * 512, 8)},

	/* NUC122 */
	{"NUC122LC1AN", 0x00012208, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC122LD2AN", 0x00012204, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC122SC1AN", 0x00012226, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC122SD2AN", 0x00012222, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC122ZC1AN", 0x00012235, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC122ZD2AN", 0x00012231, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* NUC123AN */
	{"NUC123LC2AN1", 0x00012325, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123LD4AN0", 0x00012335, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123SC2AN1", 0x00012305, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123SD4AN0", 0x00012315, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123ZC2AN1", 0x00012345, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123ZD4AN0", 0x00012355, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* NUC123AE */
	{"NUC123LC2AE1", 0x10012325, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123LD4AE0", 0x10012335, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123SC2AE1", 0x10012305, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123SD4AE0", 0x10012315, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123ZC2AE1", 0x10012345, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC123ZD4AE0", 0x10012355, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* NUC131AE */
	{"NUC131LC2AE", 0x10013103, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC131LD2AE", 0x10013100, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC131SC2AE", 0x10013113, NUMICRO_BANKS_GENERAL(32 * 1024, 4 * 1024, 4 * 1024, 8)},
	{"NUC131SD2AE", 0x10013110, NUMICRO_BANKS_GENERAL(64 * 1024, 4 * 1024, 4 * 1024, 8)},

	/* NUC200/220AN */
	{"NUC200LC2AN", 0x00020007, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC200LD2AN", 0x00020004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC200LE3AN", 0x00020000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC200SC2AN", 0x00020034, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC200SD2AN", 0x00020031, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC200SE3AN", 0x00020027, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC200VE3AN", 0x00020018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC220LC2AN", 0x00022007, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC220LD2AN", 0x00022004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC220LE3AN", 0x00022000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC220SC2AN", 0x00022034, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC220SD2AN", 0x00022031, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 4 * 1024, 8)},
	{"NUC220SE3AN", 0x00022027, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},
	{"NUC220VE3AN", 0x00022018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 4 * 1024, 8)},

	/* NUC230/240AE */
	{"NUC230LC2AE", 0x10023007, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC230LD2AE", 0x10023004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC230LE3AE", 0x10023000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"NUC230SC2AE", 0x10023034, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC230SD2AE", 0x10023031, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC230SE3AE", 0x10023027, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"NUC230VE3AE", 0x10023018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"NUC240LC2AE", 0x10024007, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC240LD2AE", 0x10024004, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC240LE3AE", 0x10024000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"NUC240SC2AE", 0x10024034, NUMICRO_BANKS_GENERAL(32 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC240SD2AE", 0x10024031, NUMICRO_BANKS_GENERAL(64 * 1024,  4 * 1024, 8 * 1024, 8)},
	{"NUC240SE3AE", 0x10024027, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},
	{"NUC240VE3AE", 0x10024018, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 8 * 1024, 8)},

	{"UNKNOWN", 0x00000000, NUMICRO_BANKS_GENERAL(128 * 1024, 0 * 1024, 16 * 1024, 8)},
};

/* Private bank information for NuMicro. */
struct  numicro_flash_bank {
	struct working_area *write_algorithm;
	bool probed;
	const struct numicro_cpu_type *cpu;
};

/* Private methods */
static int numicro_reg_unlock(struct target *target)
{
	uint32_t is_protected;
	int retval = ERROR_OK;

	/* Check to see if NUC is register unlocked or not */
	retval = target_read_u32(target, NUMICRO_SYS_WRPROT, &is_protected);
	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("protected = 0x%08" PRIx32 "", is_protected);
	if (is_protected == 0) {	/* means protected - so unlock it */
		/* unlock flash registers */
		retval = target_write_u32(target, NUMICRO_SYS_WRPROT, REG_KEY1);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, NUMICRO_SYS_WRPROT, REG_KEY2);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, NUMICRO_SYS_WRPROT, REG_KEY3);
		if (retval != ERROR_OK)
			return retval;
	}
	/* Check that unlock worked */
	retval = target_read_u32(target, NUMICRO_SYS_WRPROT, &is_protected);
	if (retval != ERROR_OK)
		return retval;

	if (is_protected == 1) {	/* means unprotected */
		LOG_DEBUG("protection removed");
	} else {
		LOG_DEBUG("still protected!!");
	}

	return ERROR_OK;
}

static int numicro_init_isp(struct target *target)
{
	uint32_t reg_stat;
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = numicro_reg_unlock(target);
	if (retval != ERROR_OK)
		return retval;

	/* Enable ISP/SRAM/TICK Clock */
	retval = target_read_u32(target, NUMICRO_SYSCLK_AHBCLK, &reg_stat);
	if (retval != ERROR_OK)
		return retval;

	reg_stat |= AHBCLK_ISP_EN | AHBCLK_SRAM_EN | AHBCLK_TICK_EN;
	retval = target_write_u32(target, NUMICRO_SYSCLK_AHBCLK, reg_stat);
	if (retval != ERROR_OK)
		return retval;

	/* Enable ISP */
	retval = target_read_u32(target, NUMICRO_FLASH_ISPCON, &reg_stat);
	if (retval != ERROR_OK)
		return retval;

	reg_stat |= ISPCON_ISPFF | ISPCON_LDUEN | ISPCON_APUEN | ISPCON_CFGUEN | ISPCON_ISPEN;
	retval = target_write_u32(target, NUMICRO_FLASH_ISPCON, reg_stat);
	if (retval != ERROR_OK)
		return retval;

	/* Write one to undocumented flash control register */
	retval = target_write_u32(target, NUMICRO_FLASH_CHEAT, 1);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static uint32_t numicro_fmc_cmd(struct target *target, uint32_t cmd, uint32_t addr, uint32_t wdata, uint32_t *rdata)
{
	uint32_t timeout, status;
	int retval = ERROR_OK;

	retval = target_write_u32(target, NUMICRO_FLASH_ISPCMD, cmd);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, NUMICRO_FLASH_ISPDAT, wdata);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, NUMICRO_FLASH_ISPADR, addr);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, NUMICRO_FLASH_ISPTRG, ISPTRG_ISPGO);
	if (retval != ERROR_OK)
		return retval;

	/* Wait for busy to clear - check the GO flag */
	timeout = 100;
	for (;;) {
		retval = target_read_u32(target, NUMICRO_FLASH_ISPTRG, &status);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("status: 0x%" PRIx32 "", status);
		if ((status & (ISPTRG_ISPGO)) == 0)
			break;
		if (timeout-- <= 0) {
			LOG_DEBUG("timed out waiting for flash");
			return ERROR_FAIL;
		}
		busy_sleep(1);	/* can use busy sleep for short times. */
	}

	retval = target_read_u32(target, NUMICRO_FLASH_ISPDAT, rdata);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}


/* NuMicro Program-LongWord Microcodes */
static const uint8_t numicro_flash_write_code[] = {
				/* Params:
				 * r0 - workarea buffer / result
				 * r1 - target address
				 * r2 - wordcount
				 * Clobbered:
				 * r4 - tmp
				 * r5 - tmp
				 * r6 - tmp
				 * r7 - tmp
				 */

				/* .L1: */
						/* for(register uint32_t i=0;i<wcount;i++){ */
	0x04, 0x1C,				/* mov    r4, r0          */
	0x00, 0x23,				/* mov    r3, #0          */
				/* .L2: */
	0x0D, 0x1A,				/* sub    r5, r1, r0      */
	0x67, 0x19,				/* add    r7, r4, r7      */
	0x93, 0x42,				/* cmp	  r3, r2		  */
	0x0C, 0xD0,				/* beq    .L7             */
				/* .L4: */
						/* NUMICRO_FLASH_ISPADR = faddr; */
	0x08, 0x4E,				/* ldr	r6, .L8           */
	0x37, 0x60,				/* str	r7, [r6]          */
						/* NUMICRO_FLASH_ISPDAT = *pLW; */
	0x80, 0xCC,				/* ldmia	r4!, {r7}     */
	0x08, 0x4D,				/* ldr	r5, .L8+4         */
	0x2F, 0x60,				/* str	r7, [r5]		  */
							/* faddr += 4; */
							/* pLW++; */
							/*  Trigger write action  */
						/* NUMICRO_FLASH_ISPTRG = ISPTRG_ISPGO; */
	0x08, 0x4D,				/* ldr	r5, .L8+8         */
	0x01, 0x26,				/* mov	r6, #1            */
	0x2E, 0x60,				/* str	r6, [r5]          */
				/* .L3: */
						/* while((NUMICRO_FLASH_ISPTRG & ISPTRG_ISPGO) == ISPTRG_ISPGO){}; */
	0x2F, 0x68,				/* ldr	r7, [r5]          */
	0xFF, 0x07,				/* lsl	r7, r7, #31       */
	0xFC, 0xD4,				/* bmi	.L3               */

	0x01, 0x33,				/* add	r3, r3, #1        */
	0xEE, 0xE7,				/* b	.L2               */
				/* .L7: */
						/* return (NUMICRO_FLASH_ISPCON & ISPCON_ISPFF); */
	0x05, 0x4B,				/* ldr	r3, .L8+12        */
	0x18, 0x68,				/* ldr	r0, [r3]          */
	0x40, 0x21,				/* mov	r1, #64           */
	0x08, 0x40,				/* and	r0, r1            */
				/* .L9: */
	0x00, 0xBE,				/* bkpt    #0             */
				/* .L8: */
	0x04, 0xC0, 0x00, 0x50,/* .word	1342226436    */
	0x08, 0xC0, 0x00, 0x50,/* .word	1342226440    */
	0x10, 0xC0, 0x00, 0x50,/* .word	1342226448    */
	0x00, 0xC0, 0x00, 0x50 /* .word	1342226432    */
};
/* Program LongWord Block Write */
static int numicro_writeblock(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t buffer_size = 1024;		/* Default minimum value */
	struct working_area *write_algorithm;
	struct working_area *source;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[3];
	struct armv7m_algorithm armv7m_info;
	int retval = ERROR_OK;

	/* Params:
	 * r0 - workarea buffer / result
	 * r1 - target address
	 * r2 - wordcount
	 * Clobbered:
	 * r4 - tmp
	 * r5 - tmp
	 * r6 - tmp
	 * r7 - tmp
	 */

	/* Increase buffer_size if needed */
	if (buffer_size < (target->working_area_size/2))
		buffer_size = (target->working_area_size/2);

	/* check code alignment */
	if (offset & 0x1) {
		LOG_WARNING("offset 0x%" PRIx32 " breaks required 2-byte alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* allocate working area with flash programming code */
	if (target_alloc_working_area(target, sizeof(numicro_flash_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	retval = target_write_buffer(target, write_algorithm->address,
		sizeof(numicro_flash_write_code), numicro_flash_write_code);
	if (retval != ERROR_OK)
		return retval;

	/* memory buffer */
	while (target_alloc_working_area(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 4;
		if (buffer_size <= 256) {
			/* free working area, write algorithm already allocated */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("No large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* *pLW (*buffer) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);    /* faddr */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);    /* number of words to program */

	/* write code buffer and use Flash programming code within NuMicro     */
	/* Set breakpoint to 0 with time-out of 1000 ms                        */
	while (count > 0) {
		uint32_t thisrun_count = (count > (buffer_size / 4)) ? (buffer_size / 4) : count;

		retval = target_write_buffer(target, source->address, thisrun_count * 4, buffer);
		if (retval != ERROR_OK)
			break;

		buf_set_u32(reg_params[0].value, 0, 32, source->address);
		buf_set_u32(reg_params[1].value, 0, 32, address);
		buf_set_u32(reg_params[2].value, 0, 32, thisrun_count);

		retval = target_run_algorithm(target, 0, NULL, 3, reg_params,
				write_algorithm->address, 0, 100000, &armv7m_info);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error executing NuMicro Flash programming algorithm");
			retval = ERROR_FLASH_OPERATION_FAILED;
			break;
		}

		buffer  += thisrun_count * 4;
		address += thisrun_count * 4;
		count   -= thisrun_count;
	}

	target_free_working_area(target, source);
	target_free_working_area(target, write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	return retval;
}

/* Flash Lock checking - examines the lock bit. */
static int numicro_protect_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	uint32_t set, config[2];
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Nuvoton NuMicro: Flash Lock Check...");

	retval = numicro_init_isp(target);
	if (retval != ERROR_OK)
		return retval;

	/* Read CONFIG0,CONFIG1 */
	numicro_fmc_cmd(target, ISPCMD_READ, NUMICRO_CONFIG0, 0 , &config[0]);
	numicro_fmc_cmd(target, ISPCMD_READ, NUMICRO_CONFIG1, 0 , &config[1]);

	LOG_DEBUG("CONFIG0: 0x%" PRIx32 ",CONFIG1: 0x%" PRIx32 "", config[0], config[1]);

	if ((config[0] & (1<<7)) == 0)
		LOG_INFO("CBS=0: Boot From LPROM");
	else
		LOG_INFO("CBS=1: Boot From APROM");

	if ((config[0] & CONFIG0_LOCK_MASK) == 0) {

		LOG_INFO("Flash is secure locked!");
		LOG_INFO("TO UNLOCK FLASH,EXECUTE chip_erase COMMAND!!");
		set = 1;
	} else {
		LOG_INFO("Flash is not locked!");
	    set = 0;
	}

	for (unsigned int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = set;

	return ERROR_OK;
}


static int numicro_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	struct target *target = bank->target;
	uint32_t timeout, status;
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Nuvoton NuMicro: Sector Erase ... (%u to %u)", first, last);

	retval = numicro_init_isp(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, NUMICRO_FLASH_ISPCMD, ISPCMD_ERASE);
	if (retval != ERROR_OK)
		return retval;

	for (unsigned int i = first; i <= last; i++) {
		LOG_DEBUG("erasing sector %u at address " TARGET_ADDR_FMT, i,
				bank->base + bank->sectors[i].offset);
		retval = target_write_u32(target, NUMICRO_FLASH_ISPADR, bank->base + bank->sectors[i].offset);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, NUMICRO_FLASH_ISPTRG, ISPTRG_ISPGO); /* This is the only bit available */
		if (retval != ERROR_OK)
			return retval;

		/* wait for busy to clear - check the GO flag */
		timeout = 100;
		for (;;) {
			retval = target_read_u32(target, NUMICRO_FLASH_ISPTRG, &status);
			if (retval != ERROR_OK)
				return retval;
			LOG_DEBUG("status: 0x%" PRIx32 "", status);
			if (status == 0)
				break;
			if (timeout-- <= 0) {
				LOG_DEBUG("timed out waiting for flash");
				return ERROR_FAIL;
			}
			busy_sleep(1);	/* can use busy sleep for short times. */
		}

		/* check for failure */
		retval = target_read_u32(target, NUMICRO_FLASH_ISPCON, &status);
		if (retval != ERROR_OK)
			return retval;
		if ((status & ISPCON_ISPFF) != 0) {
			LOG_DEBUG("failure: 0x%" PRIx32 "", status);
			/* if bit is set, then must write to it to clear it. */
			retval = target_write_u32(target, NUMICRO_FLASH_ISPCON, (status | ISPCON_ISPFF));
			if (retval != ERROR_OK)
				return retval;
		}
	}

	/* done, */
	LOG_DEBUG("Erase done.");

	return ERROR_OK;
}

/* The write routine stub. */
static int numicro_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t timeout, status;
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Nuvoton NuMicro: Flash Write ...");

	retval = numicro_init_isp(target);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, NUMICRO_FLASH_ISPCMD, ISPCMD_WRITE);
	if (retval != ERROR_OK)
		return retval;

	assert(offset % 4 == 0);
	assert(count % 4 == 0);

	uint32_t words_remaining = count / 4;

	/* try using a block write */
	retval = numicro_writeblock(bank, buffer, offset, words_remaining);

	if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
		/* if block write failed (no sufficient working area),
		 * we use normal (slow) single word accesses */
		LOG_WARNING("couldn't use block writes, falling back to single "
			"memory accesses");

		/* program command */
		for (uint32_t i = 0; i < count; i += 4) {

			LOG_DEBUG("write longword @ %08" PRIX32, offset + i);

			retval = target_write_u32(target, NUMICRO_FLASH_ISPADR, bank->base + offset + i);
			if (retval != ERROR_OK)
				return retval;
			retval = target_write_memory(target, NUMICRO_FLASH_ISPDAT, 4, 1, buffer + i);
			if (retval != ERROR_OK)
				return retval;
			retval = target_write_u32(target, NUMICRO_FLASH_ISPTRG, ISPTRG_ISPGO);
			if (retval != ERROR_OK)
				return retval;

			/* wait for busy to clear - check the GO flag */
			timeout = 100;
			for (;;) {
				retval = target_read_u32(target, NUMICRO_FLASH_ISPTRG, &status);
				if (retval != ERROR_OK)
					return retval;
				LOG_DEBUG("status: 0x%" PRIx32 "", status);
				if (status == 0)
					break;
				if (timeout-- <= 0) {
					LOG_DEBUG("timed out waiting for flash");
					return ERROR_FAIL;
				}
				busy_sleep(1);	/* can use busy sleep for short times. */
			}

		}
	}

	/* check for failure */
	retval = target_read_u32(target, NUMICRO_FLASH_ISPCON, &status);
	if (retval != ERROR_OK)
		return retval;
	if ((status & ISPCON_ISPFF) != 0) {
		LOG_DEBUG("failure: 0x%" PRIx32 "", status);
		/* if bit is set, then must write to it to clear it. */
		retval = target_write_u32(target, NUMICRO_FLASH_ISPCON, (status | ISPCON_ISPFF));
		if (retval != ERROR_OK)
			return retval;
	} else {
		LOG_DEBUG("Write OK");
	}

	/* done. */
	LOG_DEBUG("Write done.");

	return ERROR_OK;
}

static int numicro_get_cpu_type(struct target *target, const struct numicro_cpu_type **cpu)
{
	uint32_t part_id;
	int retval = ERROR_OK;

	/* Read NuMicro PartID */
	retval = target_read_u32(target, NUMICRO_SYS_BASE, &part_id);
	if (retval != ERROR_OK) {
		LOG_WARNING("NuMicro flash driver: Failed to Get PartID\n");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	LOG_INFO("Device ID: 0x%08" PRIx32 "", part_id);
	/* search part numbers */
	for (size_t i = 0; i < ARRAY_SIZE(numicro_parts); i++) {
		if (part_id == numicro_parts[i].partid) {
			*cpu = &numicro_parts[i];
			LOG_INFO("Device Name: %s", (*cpu)->partname);
			return ERROR_OK;
		}
	}

	return ERROR_FAIL;
}

static int numicro_get_flash_size(struct flash_bank *bank, const struct numicro_cpu_type *cpu, uint32_t *flash_size)
{
	for (size_t i = 0; i < cpu->n_banks; i++) {
		if (bank->base == cpu->bank[i].base) {
			*flash_size = cpu->bank[i].size;
			LOG_INFO("bank base = " TARGET_ADDR_FMT ", size = 0x%08"
					PRIx32, bank->base, *flash_size);
			return ERROR_OK;
		}
	}
	return ERROR_FLASH_OPERATION_FAILED;
}

static int numicro_probe(struct flash_bank *bank)
{
	uint32_t flash_size, offset = 0;
	int num_pages;
	const struct numicro_cpu_type *cpu;
	struct target *target = bank->target;
	int retval = ERROR_OK;

	retval = numicro_get_cpu_type(target, &cpu);
	if (retval != ERROR_OK) {
		LOG_WARNING("NuMicro flash driver: Failed to detect a known part\n");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	retval = numicro_get_flash_size(bank, cpu, &flash_size);
	if (retval != ERROR_OK) {
		LOG_WARNING("NuMicro flash driver: Failed to detect flash size\n");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	num_pages = flash_size / NUMICRO_PAGESIZE;

	bank->num_sectors = num_pages;
	bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);
	bank->size = flash_size;

	for (int i = 0; i < num_pages; i++) {
		bank->sectors[i].offset = offset;
		bank->sectors[i].size = NUMICRO_PAGESIZE;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
		offset += NUMICRO_PAGESIZE;
	}

	struct numicro_flash_bank *numicro_info = bank->driver_priv;
	numicro_info->probed = true;
	numicro_info->cpu = cpu;
	LOG_DEBUG("Nuvoton NuMicro: Probed ...");

	return ERROR_OK;
}

/* Standard approach to autoprobing. */
static int numicro_auto_probe(struct flash_bank *bank)
{
	struct numicro_flash_bank *numicro_info = bank->driver_priv;
	if (numicro_info->probed)
		return ERROR_OK;
	return numicro_probe(bank);
}


/* This is the function called in the config file. */
FLASH_BANK_COMMAND_HANDLER(numicro_flash_bank_command)
{
	struct numicro_flash_bank *bank_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	LOG_DEBUG("add flash_bank numicro %s", bank->name);

	bank_info = malloc(sizeof(struct numicro_flash_bank));

	memset(bank_info, 0, sizeof(struct numicro_flash_bank));

	bank->driver_priv = bank_info;
	bank->write_start_alignment = bank->write_end_alignment = 4;

	return ERROR_OK;

}

COMMAND_HANDLER(numicro_handle_read_isp_command)
{
	uint32_t address;
	uint32_t ispdat;
	int retval = ERROR_OK;

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], address);

	struct target *target = get_current_target(CMD_CTX);

	retval = numicro_init_isp(target);
	if (retval != ERROR_OK)
		return retval;

	retval = numicro_fmc_cmd(target, ISPCMD_READ, address, 0, &ispdat);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("0x%08" PRIx32 ": 0x%08" PRIx32, address, ispdat);

	return ERROR_OK;
}

COMMAND_HANDLER(numicro_handle_write_isp_command)
{
	uint32_t address;
	uint32_t ispdat, rdat;
	int retval = ERROR_OK;

	if (CMD_ARGC != 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], address);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], ispdat);

	struct target *target = get_current_target(CMD_CTX);

	retval = numicro_init_isp(target);
	if (retval != ERROR_OK)
		return retval;

	retval = numicro_fmc_cmd(target, ISPCMD_WRITE, address, ispdat, &rdat);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("0x%08" PRIx32 ": 0x%08" PRIx32, address, ispdat);
	return ERROR_OK;
}

COMMAND_HANDLER(numicro_handle_chip_erase_command)
{
	int retval = ERROR_OK;
	uint32_t rdat;

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	retval = numicro_init_isp(target);
	if (retval != ERROR_OK)
		return retval;

	retval = numicro_fmc_cmd(target, ISPCMD_CHIPERASE, 0, 0, &rdat);
	if (retval != ERROR_OK) {
		command_print(CMD, "numicro chip_erase failed");
		return retval;
	}

	command_print(CMD, "numicro chip_erase complete");

	return ERROR_OK;
}

static const struct command_registration numicro_exec_command_handlers[] = {
	{
		.name = "read_isp",
		.handler = numicro_handle_read_isp_command,
		.usage = "address",
		.mode = COMMAND_EXEC,
		.help = "read flash through ISP.",
	},
	{
		.name = "write_isp",
		.handler = numicro_handle_write_isp_command,
		.usage = "address value",
		.mode = COMMAND_EXEC,
		.help = "write flash through ISP.",
	},
	{
		.name = "chip_erase",
		.handler = numicro_handle_chip_erase_command,
		.mode = COMMAND_EXEC,
		.help = "chip erase through ISP.",
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration numicro_command_handlers[] = {
	{
		.name = "numicro",
		.mode = COMMAND_ANY,
		.help = "numicro flash command group",
		.usage = "",
		.chain = numicro_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

const struct flash_driver numicro_flash = {
	.name = "numicro",
	.commands = numicro_command_handlers,
	.flash_bank_command = numicro_flash_bank_command,
	.erase = numicro_erase,
	.write = numicro_write,
	.read = default_flash_read,
	.probe = numicro_probe,
	.auto_probe = numicro_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = numicro_protect_check,
	.free_driver_priv = default_flash_free_driver_priv,
};
