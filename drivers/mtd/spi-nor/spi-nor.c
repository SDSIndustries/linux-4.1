/*
 * Based on m25p80.c, by Mike Lavender (mike@steroidmicros.com), with
 * influence from lart.c (Abraham Van Der Merwe) and mtd_dataflash.c
 *
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/math64.h>

#include <linux/mtd/cfi.h>
#include <linux/mtd/mtd.h>
#include <linux/of_platform.h>
#include <linux/spi/flash.h>
#include <linux/mtd/spi-nor.h>

/* Define max times to check status register before we give up. */
#define	MAX_READY_WAIT_JIFFIES	(40 * HZ) /* M25P16 specs 40s max chip erase */

#define SPI_NOR_MAX_ID_LEN	6

struct flash_info {
	/*
	 * This array stores the ID bytes.
	 * The first three bytes are the JEDIC ID.
	 * JEDEC ID zero means "no ID" (mostly older chips).
	 */
	u8		id[SPI_NOR_MAX_ID_LEN];
	u8		id_len;

	/* The size listed here is what works with SPINOR_OP_SE, which isn't
	 * necessarily called a "sector" by the vendor.
	 */
	unsigned	sector_size;
	u16		n_sectors;

	u16		page_size;
	u16		addr_width;

	u16		flags;
#define	SECT_4K			0x01	/* SPINOR_OP_BE_4K works uniformly */
#define	SPI_NOR_NO_ERASE	0x02	/* No erase command needed */
#define	SST_WRITE		0x04	/* use SST byte programming */
#define	SPI_NOR_NO_FR		0x08	/* Can't do fastread */
#define	SECT_4K_PMC		0x10	/* SPINOR_OP_BE_4K_PMC works uniformly */
#define	SPI_NOR_DUAL_READ	0x20    /* Flash supports Dual Read */
#define	SPI_NOR_QUAD_READ	0x40    /* Flash supports Quad Read */
#define	USE_FSR			0x80	/* use flag status register */
};

#define JEDEC_MFR(info)	((info)->id[0])

struct read_id_config {
	enum read_mode		mode;
	enum spi_protocol	proto;
};

static const struct spi_device_id *spi_nor_match_id(const char *name);

/*
 * Read the status register, returning its value in the location
 * Return the status register value.
 * Returns negative if error occurred.
 */
static int read_sr(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_RDSR, &val, 1);
	if (ret < 0) {
		pr_err("error %d reading SR\n", (int) ret);
		return ret;
	}

	return val;
}

/*
 * Read the flag status register, returning its value in the location
 * Return the status register value.
 * Returns negative if error occurred.
 */
static int read_fsr(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_RDFSR, &val, 1);
	if (ret < 0) {
		pr_err("error %d reading FSR\n", ret);
		return ret;
	}

	return val;
}

/*
 * Read configuration register, returning its value in the
 * location. Return the configuration register value.
 * Returns negative if error occured.
 */
static int read_cr(struct spi_nor *nor)
{
	int ret;
	u8 val;

	ret = nor->read_reg(nor, SPINOR_OP_RDCR, &val, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error %d reading CR\n", ret);
		return ret;
	}

	return val;
}

/*
 * Write status register 1 byte
 * Returns negative if error occurred.
 */
static inline int write_sr(struct spi_nor *nor, u8 val)
{
	nor->cmd_buf[0] = val;
	return nor->write_reg(nor, SPINOR_OP_WRSR, nor->cmd_buf, 1, 0);
}

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
static inline int write_enable(struct spi_nor *nor)
{
	return nor->write_reg(nor, SPINOR_OP_WREN, NULL, 0, 0);
}

/*
 * Send write disble instruction to the chip.
 */
static inline int write_disable(struct spi_nor *nor)
{
	return nor->write_reg(nor, SPINOR_OP_WRDI, NULL, 0, 0);
}

static inline struct spi_nor *mtd_to_spi_nor(struct mtd_info *mtd)
{
	return mtd->priv;
}

/* Enable/disable 4-byte addressing mode. */
static inline int set_4byte(struct spi_nor *nor, struct flash_info *info,
			    int enable)
{
	int status;
	bool need_wren = false;
	u8 cmd;

	switch (JEDEC_MFR(info)) {
	case CFI_MFR_ST: /* Micron, actually */
		/* Some Micron need WREN command; all will accept it */
		need_wren = true;
	case CFI_MFR_MACRONIX:
	case 0xEF /* winbond */:
		if (need_wren)
			write_enable(nor);

		cmd = enable ? SPINOR_OP_EN4B : SPINOR_OP_EX4B;
		status = nor->write_reg(nor, cmd, NULL, 0, 0);
		if (need_wren)
			write_disable(nor);

		return status;
	default:
		/* Spansion style */
		nor->cmd_buf[0] = enable << 7;
		return nor->write_reg(nor, SPINOR_OP_BRWR, nor->cmd_buf, 1, 0);
	}
}
static inline int spi_nor_sr_ready(struct spi_nor *nor)
{
	int sr = read_sr(nor);
	if (sr < 0)
		return sr;
	else
		return !(sr & SR_WIP);
}

static inline int spi_nor_fsr_ready(struct spi_nor *nor)
{
	int fsr = read_fsr(nor);
	if (fsr < 0)
		return fsr;
	else
		return fsr & FSR_READY;
}

static int spi_nor_ready(struct spi_nor *nor)
{
	int sr, fsr;
	sr = spi_nor_sr_ready(nor);
	if (sr < 0)
		return sr;
	fsr = nor->flags & SNOR_F_USE_FSR ? spi_nor_fsr_ready(nor) : 1;
	if (fsr < 0)
		return fsr;
	return sr && fsr;
}

/*
 * Service routine to read status register until ready, or timeout occurs.
 * Returns non-zero if error.
 */
static int spi_nor_wait_till_ready(struct spi_nor *nor)
{
	unsigned long deadline;
	int timeout = 0, ret;

	deadline = jiffies + MAX_READY_WAIT_JIFFIES;

	while (!timeout) {
		if (time_after_eq(jiffies, deadline))
			timeout = 1;

		ret = spi_nor_ready(nor);
		if (ret < 0)
			return ret;
		if (ret)
			return 0;

		cond_resched();
	}

	dev_err(nor->dev, "flash operation timed out\n");

	return -ETIMEDOUT;
}

/*
 * Erase the whole flash memory
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int erase_chip(struct spi_nor *nor)
{
	dev_dbg(nor->dev, " %lldKiB\n", (long long)(nor->mtd->size >> 10));

	return nor->write_reg(nor, SPINOR_OP_CHIP_ERASE, NULL, 0, 0);
}

static int spi_nor_lock_and_prep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	int ret = 0;

	mutex_lock(&nor->lock);

	if (nor->prepare) {
		ret = nor->prepare(nor, ops);
		if (ret) {
			dev_err(nor->dev, "failed in the preparation.\n");
			mutex_unlock(&nor->lock);
			return ret;
		}
	}
	return ret;
}

static void spi_nor_unlock_and_unprep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	if (nor->unprepare)
		nor->unprepare(nor, ops);
	mutex_unlock(&nor->lock);
}

/*
 * Erase an address range on the nor chip.  The address range may extend
 * one or more erase sectors.  Return an error is there is a problem erasing.
 */
static int spi_nor_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	u32 addr, len;
	uint32_t rem;
	int ret;

	dev_dbg(nor->dev, "at 0x%llx, len %lld\n", (long long)instr->addr,
			(long long)instr->len);

	div_u64_rem(instr->len, mtd->erasesize, &rem);
	if (rem)
		return -EINVAL;

	addr = instr->addr;
	len = instr->len;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_ERASE);
	if (ret)
		return ret;

	/* whole-chip erase? */
	if (len == mtd->size) {
		write_enable(nor);

		if (erase_chip(nor)) {
			ret = -EIO;
			goto erase_err;
		}

		ret = spi_nor_wait_till_ready(nor);
		if (ret)
			goto erase_err;

	/* REVISIT in some cases we could speed up erasing large regions
	 * by using SPINOR_OP_SE instead of SPINOR_OP_BE_4K.  We may have set up
	 * to use "small sector erase", but that's not always optimal.
	 */

	/* "sector"-at-a-time erase */
	} else {
		while (len) {
			write_enable(nor);

			if (nor->erase(nor, addr)) {
				ret = -EIO;
				goto erase_err;
			}

			addr += mtd->erasesize;
			len -= mtd->erasesize;

			ret = spi_nor_wait_till_ready(nor);
			if (ret)
				goto erase_err;
		}
	}

	write_disable(nor);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_ERASE);

	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);

	return ret;

erase_err:
	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_ERASE);
	instr->state = MTD_ERASE_FAILED;
	return ret;
}

static int stm_lock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct mtd_info *mtd = nor->mtd;
	uint32_t offset = ofs;
	uint8_t status_old, status_new;
	int ret = 0;

	status_old = read_sr(nor);

	if (offset < mtd->size - (mtd->size / 2))
		status_new = status_old | SR_BP2 | SR_BP1 | SR_BP0;
	else if (offset < mtd->size - (mtd->size / 4))
		status_new = (status_old & ~SR_BP0) | SR_BP2 | SR_BP1;
	else if (offset < mtd->size - (mtd->size / 8))
		status_new = (status_old & ~SR_BP1) | SR_BP2 | SR_BP0;
	else if (offset < mtd->size - (mtd->size / 16))
		status_new = (status_old & ~(SR_BP0 | SR_BP1)) | SR_BP2;
	else if (offset < mtd->size - (mtd->size / 32))
		status_new = (status_old & ~SR_BP2) | SR_BP1 | SR_BP0;
	else if (offset < mtd->size - (mtd->size / 64))
		status_new = (status_old & ~(SR_BP2 | SR_BP0)) | SR_BP1;
	else
		status_new = (status_old & ~(SR_BP2 | SR_BP1)) | SR_BP0;

	/* Only modify protection if it will not unlock other areas */
	if ((status_new & (SR_BP2 | SR_BP1 | SR_BP0)) >
				(status_old & (SR_BP2 | SR_BP1 | SR_BP0))) {
		write_enable(nor);
		ret = write_sr(nor, status_new);
	}

	return ret;
}

static int stm_unlock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct mtd_info *mtd = nor->mtd;
	uint32_t offset = ofs;
	uint8_t status_old, status_new;
	int ret = 0;

	status_old = read_sr(nor);

	if (offset+len > mtd->size - (mtd->size / 64))
		status_new = status_old & ~(SR_BP2 | SR_BP1 | SR_BP0);
	else if (offset+len > mtd->size - (mtd->size / 32))
		status_new = (status_old & ~(SR_BP2 | SR_BP1)) | SR_BP0;
	else if (offset+len > mtd->size - (mtd->size / 16))
		status_new = (status_old & ~(SR_BP2 | SR_BP0)) | SR_BP1;
	else if (offset+len > mtd->size - (mtd->size / 8))
		status_new = (status_old & ~SR_BP2) | SR_BP1 | SR_BP0;
	else if (offset+len > mtd->size - (mtd->size / 4))
		status_new = (status_old & ~(SR_BP0 | SR_BP1)) | SR_BP2;
	else if (offset+len > mtd->size - (mtd->size / 2))
		status_new = (status_old & ~SR_BP1) | SR_BP2 | SR_BP0;
	else
		status_new = (status_old & ~SR_BP0) | SR_BP2 | SR_BP1;

	/* Only modify protection if it will not lock other areas */
	if ((status_new & (SR_BP2 | SR_BP1 | SR_BP0)) <
				(status_old & (SR_BP2 | SR_BP1 | SR_BP0))) {
		write_enable(nor);
		ret = write_sr(nor, status_new);
	}

	return ret;
}

static int spi_nor_lock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_LOCK);
	if (ret)
		return ret;

	ret = nor->flash_lock(nor, ofs, len);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_UNLOCK);
	return ret;
}

static int spi_nor_unlock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_UNLOCK);
	if (ret)
		return ret;

	ret = nor->flash_unlock(nor, ofs, len);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_LOCK);
	return ret;
}

/* Used when the "_ext_id" is two bytes at most */
#define INFO(_jedec_id, _ext_id, _sector_size, _n_sectors, _flags)	\
	((kernel_ulong_t)&(struct flash_info) {				\
		.id = {							\
			((_jedec_id) >> 16) & 0xff,			\
			((_jedec_id) >> 8) & 0xff,			\
			(_jedec_id) & 0xff,				\
			((_ext_id) >> 8) & 0xff,			\
			(_ext_id) & 0xff,				\
			},						\
		.id_len = (!(_jedec_id) ? 0 : (3 + ((_ext_id) ? 2 : 0))),	\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = 256,					\
		.flags = (_flags),					\
	})

#define INFO6(_jedec_id, _ext_id, _sector_size, _n_sectors, _flags)	\
	((kernel_ulong_t)&(struct flash_info) {				\
		.id = {							\
			((_jedec_id) >> 16) & 0xff,			\
			((_jedec_id) >> 8) & 0xff,			\
			(_jedec_id) & 0xff,				\
			((_ext_id) >> 16) & 0xff,			\
			((_ext_id) >> 8) & 0xff,			\
			(_ext_id) & 0xff,				\
			},						\
		.id_len = 6,						\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = 256,					\
		.flags = (_flags),					\
	})

#define CAT25_INFO(_sector_size, _n_sectors, _page_size, _addr_width, _flags)	\
	((kernel_ulong_t)&(struct flash_info) {				\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = (_page_size),				\
		.addr_width = (_addr_width),				\
		.flags = (_flags),					\
	})

/* NOTE: double check command sets and memory organization when you add
 * more nor chips.  This current list focusses on newer chips, which
 * have been converging on command sets which including JEDEC ID.
 */
static const struct spi_device_id spi_nor_ids[] = {
	/* Atmel -- some are (confusingly) marketed as "DataFlash" */
	{ "at25fs010",  INFO(0x1f6601, 0, 32 * 1024,   4, SECT_4K) },
	{ "at25fs040",  INFO(0x1f6604, 0, 64 * 1024,   8, SECT_4K) },

	{ "at25df041a", INFO(0x1f4401, 0, 64 * 1024,   8, SECT_4K) },
	{ "at25df321a", INFO(0x1f4701, 0, 64 * 1024,  64, SECT_4K) },
	{ "at25df641",  INFO(0x1f4800, 0, 64 * 1024, 128, SECT_4K) },

	{ "at26f004",   INFO(0x1f0400, 0, 64 * 1024,  8, SECT_4K) },
	{ "at26df081a", INFO(0x1f4501, 0, 64 * 1024, 16, SECT_4K) },
	{ "at26df161a", INFO(0x1f4601, 0, 64 * 1024, 32, SECT_4K) },
	{ "at26df321",  INFO(0x1f4700, 0, 64 * 1024, 64, SECT_4K) },

	{ "at45db081d", INFO(0x1f2500, 0, 64 * 1024, 16, SECT_4K) },

	/* EON -- en25xxx */
	{ "en25f32",    INFO(0x1c3116, 0, 64 * 1024,   64, SECT_4K) },
	{ "en25p32",    INFO(0x1c2016, 0, 64 * 1024,   64, 0) },
	{ "en25q32b",   INFO(0x1c3016, 0, 64 * 1024,   64, 0) },
	{ "en25p64",    INFO(0x1c2017, 0, 64 * 1024,  128, 0) },
	{ "en25q64",    INFO(0x1c3017, 0, 64 * 1024,  128, SECT_4K) },
	{ "en25qh128",  INFO(0x1c7018, 0, 64 * 1024,  256, 0) },
	{ "en25qh256",  INFO(0x1c7019, 0, 64 * 1024,  512, 0) },
	{ "en25s64",	INFO(0x1c3817, 0, 64 * 1024,  128, 0) },

	/* ESMT */
	{ "f25l32pa", INFO(0x8c2016, 0, 64 * 1024, 64, SECT_4K) },

	/* Everspin */
	{ "mr25h256", CAT25_INFO( 32 * 1024, 1, 256, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "mr25h10",  CAT25_INFO(128 * 1024, 1, 256, 3, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },

	/* Fujitsu */
	{ "mb85rs1mt", INFO(0x047f27, 0, 128 * 1024, 1, SPI_NOR_NO_ERASE) },

	/* GigaDevice */
	{ "gd25q32", INFO(0xc84016, 0, 64 * 1024,  64, SECT_4K) },
	{ "gd25q64", INFO(0xc84017, 0, 64 * 1024, 128, SECT_4K) },
	{ "gd25q128", INFO(0xc84018, 0, 64 * 1024, 256, SECT_4K) },

	/* Intel/Numonyx -- xxxs33b */
	{ "160s33b",  INFO(0x898911, 0, 64 * 1024,  32, 0) },
	{ "320s33b",  INFO(0x898912, 0, 64 * 1024,  64, 0) },
	{ "640s33b",  INFO(0x898913, 0, 64 * 1024, 128, 0) },

	/* Macronix */
	{ "mx25l2005a",  INFO(0xc22012, 0, 64 * 1024,   4, SECT_4K) },
	{ "mx25l4005a",  INFO(0xc22013, 0, 64 * 1024,   8, SECT_4K) },
	{ "mx25l8005",   INFO(0xc22014, 0, 64 * 1024,  16, 0) },
	{ "mx25l1606e",  INFO(0xc22015, 0, 64 * 1024,  32, SECT_4K) },
	{ "mx25l3205d",  INFO(0xc22016, 0, 64 * 1024,  64, 0) },
	{ "mx25l3255e",  INFO(0xc29e16, 0, 64 * 1024,  64, SECT_4K) },
	{ "mx25l6405d",  INFO(0xc22017, 0, 64 * 1024, 128, 0) },
	{ "mx25u6435f",  INFO(0xc22537, 0, 64 * 1024, 128, SECT_4K) },
	{ "mx25l12805d", INFO(0xc22018, 0, 64 * 1024, 256, 0) },
	{ "mx25l12855e", INFO(0xc22618, 0, 64 * 1024, 256, 0) },
	{ "mx25l25635e", INFO(0xc22019, 0, 64 * 1024, 512, 0) },
	{ "mx25l25655e", INFO(0xc22619, 0, 64 * 1024, 512, 0) },
	{ "mx66l51235l", INFO(0xc2201a, 0, 64 * 1024, 1024, SPI_NOR_QUAD_READ) },
	{ "mx66l1g55g",  INFO(0xc2261b, 0, 64 * 1024, 2048, SPI_NOR_QUAD_READ) },

	/* Micron */
	{ "n25q032",	 INFO(0x20ba16, 0, 64 * 1024,   64, SPI_NOR_QUAD_READ) },
	{ "n25q064",     INFO(0x20ba17, 0, 64 * 1024,  128, SPI_NOR_QUAD_READ) },
	{ "n25q128a11",  INFO(0x20bb18, 0, 64 * 1024,  256, SPI_NOR_QUAD_READ) },
	{ "n25q128a13",  INFO(0x20ba18, 0, 64 * 1024,  256, SPI_NOR_QUAD_READ) },
	{ "n25q256a",    INFO(0x20ba19, 0, 64 * 1024,  512, SECT_4K | SPI_NOR_QUAD_READ) },
	{ "n25q512a",    INFO(0x20bb20, 0, 64 * 1024, 1024, SECT_4K | USE_FSR | SPI_NOR_QUAD_READ) },
	{ "n25q512ax3",  INFO(0x20ba20, 0, 64 * 1024, 1024, SECT_4K | USE_FSR | SPI_NOR_QUAD_READ) },
	{ "n25q00",      INFO(0x20ba21, 0, 64 * 1024, 2048, SECT_4K | USE_FSR | SPI_NOR_QUAD_READ) },

	/* PMC */
	{ "pm25lv512",   INFO(0,        0, 32 * 1024,    2, SECT_4K_PMC) },
	{ "pm25lv010",   INFO(0,        0, 32 * 1024,    4, SECT_4K_PMC) },
	{ "pm25lq032",   INFO(0x7f9d46, 0, 64 * 1024,   64, SECT_4K) },

	/* Spansion -- single (large) sector size only, at least
	 * for the chips listed here (without boot sectors).
	 */
	{ "s25sl032p",  INFO(0x010215, 0x4d00,  64 * 1024,  64, SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ) },
	{ "s25sl064p",  INFO(0x010216, 0x4d00,  64 * 1024, 128, 0) },
	{ "s25fl256s0", INFO(0x010219, 0x4d00, 256 * 1024, 128, 0) },
	{ "s25fl256s1", INFO(0x010219, 0x4d01,  64 * 1024, 512, SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ) },
	{ "s25fl512s",  INFO(0x010220, 0x4d00, 256 * 1024, 256, SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ) },
	{ "s70fl01gs",  INFO(0x010221, 0x4d00, 256 * 1024, 256, 0) },
	{ "s25sl12800", INFO(0x012018, 0x0300, 256 * 1024,  64, 0) },
	{ "s25sl12801", INFO(0x012018, 0x0301,  64 * 1024, 256, 0) },
	{ "s25fl128s",	INFO6(0x012018, 0x4d0180, 64 * 1024, 256, SPI_NOR_QUAD_READ) },
	{ "s25fl129p0", INFO(0x012018, 0x4d00, 256 * 1024,  64, 0) },
	{ "s25fl129p1", INFO(0x012018, 0x4d01,  64 * 1024, 256, 0) },
	{ "s25sl004a",  INFO(0x010212,      0,  64 * 1024,   8, 0) },
	{ "s25sl008a",  INFO(0x010213,      0,  64 * 1024,  16, 0) },
	{ "s25sl016a",  INFO(0x010214,      0,  64 * 1024,  32, 0) },
	{ "s25sl032a",  INFO(0x010215,      0,  64 * 1024,  64, 0) },
	{ "s25sl064a",  INFO(0x010216,      0,  64 * 1024, 128, 0) },
	{ "s25fl008k",  INFO(0xef4014,      0,  64 * 1024,  16, SECT_4K) },
	{ "s25fl016k",  INFO(0xef4015,      0,  64 * 1024,  32, SECT_4K) },
	{ "s25fl064k",  INFO(0xef4017,      0,  64 * 1024, 128, SECT_4K) },
	{ "s25fl132k",  INFO(0x014016,      0,  64 * 1024,  64, 0) },

	/* SST -- large erase sizes are "overlays", "sectors" are 4K */
	{ "sst25vf040b", INFO(0xbf258d, 0, 64 * 1024,  8, SECT_4K | SST_WRITE) },
	{ "sst25vf080b", INFO(0xbf258e, 0, 64 * 1024, 16, SECT_4K | SST_WRITE) },
	{ "sst25vf016b", INFO(0xbf2541, 0, 64 * 1024, 32, SECT_4K | SST_WRITE) },
	{ "sst25vf032b", INFO(0xbf254a, 0, 64 * 1024, 64, SECT_4K | SST_WRITE) },
	{ "sst25vf064c", INFO(0xbf254b, 0, 64 * 1024, 128, SECT_4K) },
	{ "sst25wf512",  INFO(0xbf2501, 0, 64 * 1024,  1, SECT_4K | SST_WRITE) },
	{ "sst25wf010",  INFO(0xbf2502, 0, 64 * 1024,  2, SECT_4K | SST_WRITE) },
	{ "sst25wf020",  INFO(0xbf2503, 0, 64 * 1024,  4, SECT_4K | SST_WRITE) },
	{ "sst25wf040",  INFO(0xbf2504, 0, 64 * 1024,  8, SECT_4K | SST_WRITE) },
	{ "sst25wf080",  INFO(0xbf2505, 0, 64 * 1024, 16, SECT_4K | SST_WRITE) },

	/* ST Microelectronics -- newer production may have feature updates */
	{ "m25p05",  INFO(0x202010,  0,  32 * 1024,   2, 0) },
	{ "m25p10",  INFO(0x202011,  0,  32 * 1024,   4, 0) },
	{ "m25p20",  INFO(0x202012,  0,  64 * 1024,   4, 0) },
	{ "m25p40",  INFO(0x202013,  0,  64 * 1024,   8, 0) },
	{ "m25p80",  INFO(0x202014,  0,  64 * 1024,  16, 0) },
	{ "m25p16",  INFO(0x202015,  0,  64 * 1024,  32, 0) },
	{ "m25p32",  INFO(0x202016,  0,  64 * 1024,  64, 0) },
	{ "m25p64",  INFO(0x202017,  0,  64 * 1024, 128, 0) },
	{ "m25p128", INFO(0x202018,  0, 256 * 1024,  64, 0) },

	{ "m25p05-nonjedec",  INFO(0, 0,  32 * 1024,   2, 0) },
	{ "m25p10-nonjedec",  INFO(0, 0,  32 * 1024,   4, 0) },
	{ "m25p20-nonjedec",  INFO(0, 0,  64 * 1024,   4, 0) },
	{ "m25p40-nonjedec",  INFO(0, 0,  64 * 1024,   8, 0) },
	{ "m25p80-nonjedec",  INFO(0, 0,  64 * 1024,  16, 0) },
	{ "m25p16-nonjedec",  INFO(0, 0,  64 * 1024,  32, 0) },
	{ "m25p32-nonjedec",  INFO(0, 0,  64 * 1024,  64, 0) },
	{ "m25p64-nonjedec",  INFO(0, 0,  64 * 1024, 128, 0) },
	{ "m25p128-nonjedec", INFO(0, 0, 256 * 1024,  64, 0) },

	{ "m45pe10", INFO(0x204011,  0, 64 * 1024,    2, 0) },
	{ "m45pe80", INFO(0x204014,  0, 64 * 1024,   16, 0) },
	{ "m45pe16", INFO(0x204015,  0, 64 * 1024,   32, 0) },

	{ "m25pe20", INFO(0x208012,  0, 64 * 1024,  4,       0) },
	{ "m25pe80", INFO(0x208014,  0, 64 * 1024, 16,       0) },
	{ "m25pe16", INFO(0x208015,  0, 64 * 1024, 32, SECT_4K) },

	{ "m25px16",    INFO(0x207115,  0, 64 * 1024, 32, SECT_4K) },
	{ "m25px32",    INFO(0x207116,  0, 64 * 1024, 64, SECT_4K) },
	{ "m25px32-s0", INFO(0x207316,  0, 64 * 1024, 64, SECT_4K) },
	{ "m25px32-s1", INFO(0x206316,  0, 64 * 1024, 64, SECT_4K) },
	{ "m25px64",    INFO(0x207117,  0, 64 * 1024, 128, 0) },
	{ "m25px80",    INFO(0x207114,  0, 64 * 1024, 16, 0) },

	/* Winbond -- w25x "blocks" are 64K, "sectors" are 4KiB */
	{ "w25x05", INFO(0xef3010, 0, 64 * 1024,  1,  SECT_4K) },
	{ "w25x10", INFO(0xef3011, 0, 64 * 1024,  2,  SECT_4K) },
	{ "w25x20", INFO(0xef3012, 0, 64 * 1024,  4,  SECT_4K) },
	{ "w25x40", INFO(0xef3013, 0, 64 * 1024,  8,  SECT_4K) },
	{ "w25x80", INFO(0xef3014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25x16", INFO(0xef3015, 0, 64 * 1024,  32, SECT_4K) },
	{ "w25x32", INFO(0xef3016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25q32", INFO(0xef4016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25q32dw", INFO(0xef6016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25x64", INFO(0xef3017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q64", INFO(0xef4017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q64dw", INFO(0xef6017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q80", INFO(0xef5014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25q80bl", INFO(0xef4014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25q128", INFO(0xef4018, 0, 64 * 1024, 256, SECT_4K) },
	{ "w25q256", INFO(0xef4019, 0, 64 * 1024, 512, SECT_4K) },

	/* Catalyst / On Semiconductor -- non-JEDEC */
	{ "cat25c11", CAT25_INFO(  16, 8, 16, 1, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25c03", CAT25_INFO(  32, 8, 16, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25c09", CAT25_INFO( 128, 8, 32, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25c17", CAT25_INFO( 256, 8, 32, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ "cat25128", CAT25_INFO(2048, 8, 64, 2, SPI_NOR_NO_ERASE | SPI_NOR_NO_FR) },
	{ },
};

static const struct spi_device_id *spi_nor_read_id(struct spi_nor *nor,
						enum read_mode mode)
{
	int			i, tmp;
	u8			id[SPI_NOR_MAX_ID_LEN];
	const struct flash_info	*info;
	static const struct read_id_config configs[] = {
		{SPI_NOR_QUAD, SPI_PROTO_4_4_4},
		{SPI_NOR_DUAL, SPI_PROTO_2_2_2}
	};

	tmp = nor->read_reg(nor, SPINOR_OP_RDID, id, SPI_NOR_MAX_ID_LEN);
	if (tmp < 0) {
		dev_dbg(nor->dev, " error %d reading JEDEC ID\n", tmp);
		return ERR_PTR(tmp);
	}

	/* Special case for Micron/Macronix qspi nor. */
	if ((id[0] == 0xff && id[1] == 0xff && id[2] == 0xff) ||
	    (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00)) {
		for (i = 0; i < ARRAY_SIZE(configs); ++i) {
			if (configs[i].mode != mode)
				continue;

			/* Set this protocol for all commands. */
			nor->reg_proto = configs[i].proto;
			nor->read_proto = configs[i].proto;
			nor->write_proto = configs[i].proto;
			nor->erase_proto = configs[i].proto;

			/*
			 * Multiple I/O Read ID only returns the Manufacturer ID
			 * (1 byte) and the Device ID (2 bytes). So we reset the
			 * remaining bytes.
			 */
			memset(id, 0, sizeof(id));
			tmp = nor->read_reg(nor, SPINOR_OP_MIO_RDID, id, 3);
			if (tmp < 0) {
				dev_dbg(nor->dev,
					"error %d reading JEDEC ID Multi I/O\n",
					tmp);
				return ERR_PTR(tmp);
			}
		}
	}

	for (tmp = 0; tmp < ARRAY_SIZE(spi_nor_ids) - 1; tmp++) {
		info = (void *)spi_nor_ids[tmp].driver_data;
		if (info->id_len) {
			if (!memcmp(info->id, id, info->id_len))
				return &spi_nor_ids[tmp];
		}
	}
	dev_err(nor->dev, "unrecognized JEDEC id bytes: %02x, %2x, %2x\n",
		id[0], id[1], id[2]);
	return ERR_PTR(-ENODEV);
}

static int spi_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			size_t *retlen, u_char *buf)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	int ret;

	dev_dbg(nor->dev, "from 0x%08x, len %zd\n", (u32)from, len);

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_READ);
	if (ret)
		return ret;

	ret = nor->read(nor, from, len, retlen, buf);

	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_READ);
	return ret;
}

static int sst_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	size_t actual;
	int ret;

	dev_dbg(nor->dev, "to 0x%08x, len %zd\n", (u32)to, len);

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_WRITE);
	if (ret)
		return ret;

	write_enable(nor);

	nor->sst_write_second = false;

	actual = to % 2;
	/* Start write from odd address. */
	if (actual) {
		nor->program_opcode = SPINOR_OP_BP;

		/* write one byte. */
		nor->write(nor, to, 1, retlen, buf);
		ret = spi_nor_wait_till_ready(nor);
		if (ret)
			goto time_out;
	}
	to += actual;

	/* Write out most of the data here. */
	for (; actual < len - 1; actual += 2) {
		nor->program_opcode = SPINOR_OP_AAI_WP;

		/* write two bytes. */
		nor->write(nor, to, 2, retlen, buf + actual);
		ret = spi_nor_wait_till_ready(nor);
		if (ret)
			goto time_out;
		to += 2;
		nor->sst_write_second = true;
	}
	nor->sst_write_second = false;

	write_disable(nor);
	ret = spi_nor_wait_till_ready(nor);
	if (ret)
		goto time_out;

	/* Write out trailing byte if it exists. */
	if (actual != len) {
		write_enable(nor);

		nor->program_opcode = SPINOR_OP_BP;
		nor->write(nor, to, 1, retlen, buf + actual);

		ret = spi_nor_wait_till_ready(nor);
		if (ret)
			goto time_out;
		write_disable(nor);
	}
time_out:
	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_WRITE);
	return ret;
}

/*
 * Write an address range to the nor chip.  Data must be written in
 * FLASH_PAGESIZE chunks.  The address range may be any size provided
 * it is within the physical boundaries.
 */
static int spi_nor_write(struct mtd_info *mtd, loff_t to, size_t len,
	size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd_to_spi_nor(mtd);
	u32 page_offset, page_size, i;
	int ret;

	dev_dbg(nor->dev, "to 0x%08x, len %zd\n", (u32)to, len);

	ret = spi_nor_lock_and_prep(nor, SPI_NOR_OPS_WRITE);
	if (ret)
		return ret;

	write_enable(nor);

	page_offset = to & (nor->page_size - 1);

	/* do all the bytes fit onto one page? */
	if (page_offset + len <= nor->page_size) {
		nor->write(nor, to, len, retlen, buf);
	} else {
		/* the size of data remaining on the first page */
		page_size = nor->page_size - page_offset;
		nor->write(nor, to, page_size, retlen, buf);

		/* write everything in nor->page_size chunks */
		for (i = page_size; i < len; i += page_size) {
			page_size = len - i;
			if (page_size > nor->page_size)
				page_size = nor->page_size;

			ret = spi_nor_wait_till_ready(nor);
			if (ret)
				goto write_err;

			write_enable(nor);

			nor->write(nor, to + i, page_size, retlen, buf + i);
		}
	}

	ret = spi_nor_wait_till_ready(nor);
write_err:
	spi_nor_unlock_and_unprep(nor, SPI_NOR_OPS_WRITE);
	return ret;
}

/*
 * Write status Register and configuration register with 2 bytes
 * The first byte will be written to the status register, while the
 * second byte will be written to the configuration register.
 * Return negative if error occured.
 */
static int write_sr_cr(struct spi_nor *nor, u16 val)
{
	nor->cmd_buf[0] = val & 0xff;
	nor->cmd_buf[1] = (val >> 8);

	return nor->write_reg(nor, SPINOR_OP_WRSR, nor->cmd_buf, 2, 0);
}

static int macronix_dummy2code(u8 read_opcode, u8 read_dummy, u8 *dc)
{
	switch (read_opcode) {
	case SPINOR_OP_READ:
	case SPINOR_OP_READ4:
		*dc = 0;
		break;

	case SPINOR_OP_READ_FAST:
	case SPINOR_OP_READ_1_1_2:
	case SPINOR_OP_READ_1_1_4:
	case SPINOR_OP_READ4_FAST:
	case SPINOR_OP_READ4_1_1_2:
	case SPINOR_OP_READ4_1_1_4:
		switch (read_dummy) {
		case 6:
			*dc = 1;
			break;
		case 8:
			*dc = 0;
			break;
		case 10:
			*dc = 3;
			break;
		default:
			return -EINVAL;
		}
		break;

	case SPINOR_OP_READ_1_2_2:
	case SPINOR_OP_READ4_1_2_2:
		switch (read_dummy) {
		case 4:
			*dc = 0;
			break;
		case 6:
			*dc = 1;
			break;
		case 8:
			*dc = 2;
			break;
		case 10:
			*dc = 3;
		default:
			return -EINVAL;
		}
		break;

	case SPINOR_OP_READ_1_4_4:
	case SPINOR_OP_READ4_1_4_4:
		switch (read_dummy) {
		case 4:
			*dc = 1;
			break;
		case 6:
			*dc = 0;
			break;
		case 8:
			*dc = 2;
			break;
		case 10:
			*dc = 3;
		default:
			return -EINVAL;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int macronix_set_dummy_cycles(struct spi_nor *nor, u8 read_dummy)
{
	int ret, sr, cr, mask, val;
	u16 sr_cr;
	u8 dc;

	/* Convert the number of dummy cycles into Macronix DC volatile bits */
	ret = macronix_dummy2code(nor->read_opcode, read_dummy, &dc);
	if (ret)
		return ret;

	mask = GENMASK(7, 6);
	val = (dc << 6) & mask;

	cr = read_cr(nor);
	if (cr < 0) {
		dev_err(nor->dev, "error while reading the config register\n");
		return cr;
	}

	if ((cr & mask) == val) {
		nor->read_dummy = read_dummy;
		return 0;
	}

	sr = read_sr(nor);
	if (sr < 0) {
		dev_err(nor->dev, "error while reading the status register\n");
		return sr;
	}

	cr = (cr & ~mask) | val;
	sr_cr = (sr & 0xff) | ((cr & 0xff) << 8);
	write_enable(nor);
	ret = write_sr_cr(nor, sr_cr);
	if (ret) {
		dev_err(nor->dev,
			"error while writting the SR and CR registers\n");
		return ret;
	}

	ret = spi_nor_wait_till_ready(nor);
	if (ret)
		return ret;

	cr = read_cr(nor);
	if (cr < 0 || (cr & mask) != val) {
		dev_err(nor->dev, "Macronix Dummy Cycle bits not updated\n");
		return -EINVAL;
	}

	/* Save the number of dummy cycles to use with Fast Read commands */
	nor->read_dummy = read_dummy;
	return 0;
}

static int macronix_set_quad_mode(struct spi_nor *nor)
{
	/* Check whether the QPI mode is enabled. */
	if (nor->reg_proto == SPI_PROTO_4_4_4) {
		/*
		 * In QPI mode, only the Fast Read Quad I/O (0xeb) command is
		 * supported by the memory. Also the memory expects ALL commands
		 * to use the SPI 4-4-4 protocol.
		 * We already know that the SPI controller supports this
		 * protocol as we succeeded in reading the JEDEC ID with the
		 * 0xaf command and SPI-4-4-4 protocol.
		 * However, using the 0xeb command we must take care about the
		 * values sent during the dummy cycles as we don't want the
		 * memory to enter its Continuous Read (Performance Enhance)
		 * mode.
		 */
		nor->erase_proto = SPI_PROTO_4_4_4;
		nor->write_proto = SPI_PROTO_4_4_4;
		nor->read_proto = SPI_PROTO_4_4_4;
		nor->read_opcode = SPINOR_OP_READ_1_4_4;
		return macronix_set_dummy_cycles(nor, 8);
	}

	/*
	 * Use the Fast Read Quad Output 1-1-4 (0x6b) command with 8 dummy
	 * cycles (up to 133MHz for STR and 66MHz for DTR).
	 */
	nor->read_proto = SPI_PROTO_1_1_4;
	nor->read_opcode = SPINOR_OP_READ_1_1_4;
	return macronix_set_dummy_cycles(nor, 8);
}

static int macronix_set_dual_mode(struct spi_nor *nor)
{
	/*
	 * Use the Fast Read Dual Output 1-1-2 (0x3b) command with 8 dummy
	 * cycles (up to 133MHz for STR and 66MHz for DTR).
	 */
	nor->read_proto = SPI_PROTO_1_1_2;
	nor->read_opcode = SPINOR_OP_READ_1_1_2;
	return macronix_set_dummy_cycles(nor, 8);
}

static int macronix_set_single_mode(struct spi_nor *nor)
{
	u8 read_dummy;

	/*
	 * Configure 8 dummy cycles for Fast Read 1-1-1 (0x0b) command (up to
	 * 133MHz for STR and 66MHz for DTR). The Read 1-1-1 (0x03) command
	 * expects no dummy cycle.
	 * read_opcode should not be overridden here!
	 */
	switch (nor->read_opcode) {
	case SPINOR_OP_READ:
	case SPINOR_OP_READ4:
		read_dummy = 0;
		break;

	default:
		read_dummy = 8;
		break;
	}

	nor->read_proto = SPI_PROTO_1_1_1;
	return macronix_set_dummy_cycles(nor, read_dummy);
}

static inline int spansion_get_config(struct spi_nor *nor,
				      bool *quad_enabled,
				      u8 *latency_code)
{
	int cr;

	cr = read_cr(nor);
	if (cr < 0) {
		dev_err(nor->dev,
			"error while reading the configuration register\n");
		return cr;
	}

	if (quad_enabled)
		*quad_enabled = !!(cr & CR_QUAD_EN_SPAN);

	if (latency_code)
		*latency_code = (u8)((cr & GENMASK(7, 6)) >> 6);

	return 0;
}

static int spansion_quad_enable(struct spi_nor *nor)
{
	int ret;
	int quad_en = CR_QUAD_EN_SPAN << 8;

	write_enable(nor);

	ret = write_sr_cr(nor, quad_en);
	if (ret < 0) {
		dev_err(nor->dev,
			"error while writing configuration register\n");
		return -EINVAL;
	}

	/* read back and check it */
	ret = read_cr(nor);
	if (!(ret > 0 && (ret & CR_QUAD_EN_SPAN))) {
		dev_err(nor->dev, "Spansion Quad bit not set\n");
		return -EINVAL;
	}

	return 0;
}

static int spansion_set_dummy_cycles(struct spi_nor *nor, u8 latency_code)
{
	/* SDR dummy cycles */
	switch (nor->read_opcode) {
	case SPINOR_OP_READ:
	case SPINOR_OP_READ4:
		nor->read_dummy = 0;
		break;

	case SPINOR_OP_READ_FAST:
	case SPINOR_OP_READ_1_1_2:
	case SPINOR_OP_READ_1_1_4:
	case SPINOR_OP_READ4_FAST:
	case SPINOR_OP_READ4_1_1_2:
	case SPINOR_OP_READ4_1_1_4:
		nor->read_dummy = (latency_code == 3) ? 0 : 8;
		break;

	case SPINOR_OP_READ_1_2_2:
	case SPINOR_OP_READ4_1_2_2:
		switch (latency_code) {
		default:
		case 0:
		case 3:
			nor->read_dummy = 4;
			break;
		case 1:
			nor->read_dummy = 5;
			break;
		case 2:
			nor->read_dummy = 6;
			break;
		}
		break;


	case SPINOR_OP_READ_1_4_4:
	case SPINOR_OP_READ4_1_4_4:
		switch (latency_code) {
		default:
		case 0:
		case 1:
			nor->read_dummy = 4;
			break;
		case 2:
			nor->read_dummy = 5;
			break;
		case 3:
			nor->read_dummy = 1;
			break;
		}

	default:
		return -EINVAL;
	}

	return 0;
}

static int spansion_set_quad_mode(struct spi_nor *nor)
{
	bool quad_enabled;
	u8 latency_code;
	int ret;

	/*
	 * The QUAD bit of Configuration Register must be set (CR Bit1=1) for
	 * using any Quad SPI command.
	 */
	ret = spansion_get_config(nor, &quad_enabled, &latency_code);
	if (ret)
		return ret;

	/* The Quad mode should be enabled ... */
	if (!quad_enabled) {
		/* ... if not try to enable it. */
		dev_warn(nor->dev, "Spansion Quad mode disabled, enable it\n");
		ret = spansion_quad_enable(nor);
		if (ret)
			return ret;
	}

	/*
	 * Don't use the Fast Read Quad I/O (0xeb / 0xec) commands as their
	 * number of dummy cycles can not be set to a multiple of 8: some SPI
	 * controllers, especially those relying on the m25p80 driver, expect
	 * the number of dummy cycles to be a multiple of 8.
	 * Also when using a Fast Read Quad I/O command, the memory checks the
	 * value of the first mode/dummy cycles to decice whether it enters or
	 * leaves the Countinuous Read mode. We should never enter the
	 * Countinuous Read mode as the spi-nor framework doesn't support it.
	 * For all these reason, we'd rather use the Fast Read Quad Output
	 * 1-1-4 (0x6b / 0x6c) commands instead.
	 */
	nor->read_proto = SPI_PROTO_1_1_4;
	nor->read_opcode = SPINOR_OP_READ_1_1_4;
	return spansion_set_dummy_cycles(nor, latency_code);
}

static int spansion_set_dual_output(struct spi_nor *nor)
{
	u8 latency_code;
	int ret;

	/* We don't care about the quad mode status */
	ret = spansion_get_config(nor, NULL, &latency_code);
	if (ret)
		return ret;

	/*
	 * Don't use the Fast Read Dual I/O (0xbb / 0xbc) commands as their
	 * number of dummy cycles can not bet set to a multiple of 8: some SPI
	 * controllers, especially those relying on the m25p80 driver, expect
	 * the number of dummy cycles to be a multiple of 8.
	 * For this reason, w'd rather use the Fast Read Dual Output 1-1-2
	 * (0x3b / 0x3c) commands instead.
	 */
	nor->read_proto = SPI_PROTO_1_1_2;
	nor->read_opcode = SPINOR_OP_READ_1_1_2;
	return spansion_set_dummy_cycles(nor, latency_code);
}

static int spansion_set_single(struct spi_nor *nor)
{
	u8 latency_code;
	int ret;

	/* We don't care about the quad mode status */
	ret = spansion_get_config(nor, NULL, &latency_code);
	if (ret)
		return ret;

	nor->read_proto = SPI_PROTO_1_1_1;
	return spansion_set_dummy_cycles(nor, latency_code);
}

static int micron_set_dummy_cycles(struct spi_nor *nor, u8 read_dummy)
{
	u8 vcr, val, mask;
	int ret;

	/* Set bit3 (XIP) to disable the Continuous Read mode */
	mask = GENMASK(7, 4) | BIT(3);
	val = ((read_dummy << 4) | BIT(3)) & mask;

	/* Read the Volatile Configuration Register (VCR). */
	ret = nor->read_reg(nor, SPINOR_OP_RD_VCR, &vcr, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error while reading VCR register\n");
		return ret;
	}

	/* Check whether we need to update the number of dummy cycles. */
	if ((vcr & mask) == val) {
		nor->read_dummy = read_dummy;
		return 0;
	}

	/* Update the number of dummy into the VCR. */
	write_enable(nor);
	vcr = (vcr & ~mask) | val;
	ret = nor->write_reg(nor, SPINOR_OP_WR_VCR, &vcr, 1, 0);
	if (ret < 0) {
		dev_err(nor->dev, "error while writing VCR register\n");
		return ret;
	}

	ret = spi_nor_wait_till_ready(nor);
	if (ret)
		return ret;

	/* Read VCR and check it. */
	ret = nor->read_reg(nor, SPINOR_OP_RD_VCR, &vcr, 1);
	if (ret < 0 || (vcr & mask) != val) {
		dev_err(nor->dev, "Micron VCR dummy cycles not updated\n");
		return -EINVAL;
	}

	/* Save the number of dummy cycles to use with Fast Read commands */
	nor->read_dummy = read_dummy;
	return 0;
}

static int micron_set_protocol(struct spi_nor *nor, u8 mask, u8 val,
			       enum spi_protocol proto)
{
	u8 evcr;
	int ret;

	/* Read the Exhanced Volatile Configuration Register (EVCR). */
	ret = nor->read_reg(nor, SPINOR_OP_RD_EVCR, &evcr, 1);
	if (ret < 0) {
		dev_err(nor->dev, "error while reading EVCR register\n");
		return ret;
	}

	/* Check whether we need to update the protocol bits. */
	if ((evcr & mask) == val)
		return 0;

	/* Set EVCR protocol bits. */
	write_enable(nor);
	evcr = (evcr & ~mask) | val;
	ret = nor->write_reg(nor, SPINOR_OP_WD_EVCR, &evcr, 1, 0);
	if (ret < 0) {
		dev_err(nor->dev, "error while writing EVCR register\n");
		return ret;
	}

	/* Switch reg protocol now before accessing any other registers. */
	nor->reg_proto = proto;

	ret = spi_nor_wait_till_ready(nor);
	if (ret)
		return ret;

	/* Read EVCR and check it. */
	ret = nor->read_reg(nor, SPINOR_OP_RD_EVCR, &evcr, 1);
	if (ret < 0 || (evcr & mask) != val) {
		dev_err(nor->dev, "Micron EVCR protocol bits not updated\n");
		return -EINVAL;
	}

	return 0;
}

static int micron_set_quad_protocol(struct spi_nor *nor)
{
	int ret;

	/* Set Quad bit to 0 to select the Quad SPI mode. */
	ret = micron_set_protocol(nor,
				  EVCR_QUAD_EN_MICRON,
				  0,
				  SPI_PROTO_4_4_4);
	if (ret) {
		dev_err(nor->dev, "Failed to set Micron Quad SPI mode\n");
		return ret;
	}

	nor->read_proto = SPI_PROTO_4_4_4;
	nor->write_proto = SPI_PROTO_4_4_4;
	nor->erase_proto = SPI_PROTO_4_4_4;
	return 0;
}

static int micron_set_dual_protocol(struct spi_nor *nor)
{
	int ret;

	/* Set Quad/Dual bits to 10 to select the Dual SPI mode. */
	ret = micron_set_protocol(nor,
				  EVCR_QUAD_EN_MICRON | EVCR_DUAL_EN_MICRON,
				  EVCR_QUAD_EN_MICRON,
				  SPI_PROTO_2_2_2);
	if (ret) {
		dev_err(nor->dev, "Failed to set Micron Dual SPI mode\n");
		return ret;
	}

	nor->read_proto = SPI_PROTO_2_2_2;
	nor->write_proto = SPI_PROTO_2_2_2;
	nor->erase_proto = SPI_PROTO_2_2_2;
	return 0;
}

static int micron_set_extended_spi_protocol(struct spi_nor *nor)
{
	int ret;

	/* Set Quad/Dual bits to 11 to select the Extended SPI mode */
	ret = micron_set_protocol(nor,
				  EVCR_QUAD_EN_MICRON | EVCR_DUAL_EN_MICRON,
				  EVCR_QUAD_EN_MICRON | EVCR_DUAL_EN_MICRON,
				  SPI_PROTO_1_1_1);
	if (ret) {
		dev_err(nor->dev, "Failed to set Micron Extended SPI mode\n");
		return ret;
	}

	nor->write_proto = SPI_PROTO_1_1_1;
	nor->erase_proto = SPI_PROTO_1_1_1;
	return 0;
}

static int micron_set_quad_mode(struct spi_nor *nor)
{
	int ret;

	/* Check whether the Quad SPI mode is enabled. */
	if (nor->reg_proto == SPI_PROTO_4_4_4) {
		/*
		 * If here, the Quad mode should have already been enabled and
		 * is supported by the SPI controller since the memory replied
		 * to the Read ID Multiple I/O (0xaf) command in SPI 4-4-4
		 * protocol. So it might be enough to only set the read, write
		 * and erase protocols to SPI 4-4-4 but just in case...
		 */
		ret = micron_set_quad_protocol(nor);
		if (ret)
			return ret;

		/*
		 * In Quad mode, the memory doesn't make any difference between
		 * the Fast Read Quad Output 1-1-4 (0x6b) and Fast Read Quad I/O
		 * 1-4-4 (0xeb) commands: they are both processed in SPI 4-4-4
		 * protocol. The 1-4-4 command is chosen here only for debug
		 * purpose to easily detect the chosen mode when logging
		 * commands.
		 */
		nor->read_opcode = SPINOR_OP_READ_1_4_4;
		return micron_set_dummy_cycles(nor, 8);
	}

	/*
	 * Exit Dual or Quad mode if not done yet: the Fast Read Quad Output
	 * 1-1-4 (0x6b) command is also supported by the Extended SPI Protocol.
	 * We can change the mode safely as we write into a volatile register.
	 */
	ret = micron_set_extended_spi_protocol(nor);
	if (ret)
		return ret;

	/*
	 * Use the Fast Read Quad Output 1-1-4 command.
	 * Force the number of dummy cycles to 8 and disable the Continuous Read
	 * mode to prevent some drivers from using it by mistake (m25p80).
	 * We can change these settings safely as we write into a volatile
	 * register.
	 */
	nor->read_proto = SPI_PROTO_1_1_4;
	nor->read_opcode = SPINOR_OP_READ_1_1_4;
	return micron_set_dummy_cycles(nor, 8);
}

static int micron_set_dual_mode(struct spi_nor *nor)
{
	int ret;

	/* Check whether the Dual SPI mode is enabled. */
	if (nor->reg_proto == SPI_PROTO_2_2_2) {
		/*
		 * If here, the Dual mode should have already been enabled and
		 * is supported by the SPI controller since the memory replied
		 * to the Read ID Multiple I/O (0xaf) command in SPI 2-2-2
		 * protocol. So it might be enough to only set the read, write
		 * and erase protocols to SPI 2-2-2 but just in case...
		 */
		ret = micron_set_dual_protocol(nor);
		if (ret)
			return ret;

		/*
		 * In Dual mode, the memory doesn't make any difference between
		 * the Fast Read Dual Output 1-1-2 (0x3b) and Fast Read Dual I/O
		 * 1-2-2 (0xbb) commands: they are both processed in SPI 2-2-2
		 * protocol. The 1-2-2 command is chosen here only for debug
		 * purpose to easily detect the chosen mode when logging
		 * commands.
		 */
		nor->read_opcode = SPINOR_OP_READ_1_2_2;
		return micron_set_dummy_cycles(nor, 8);
	}

	/*
	 * Exit Dual or Quad mode if not done yet: the Fast Read Dual Output
	 * 1-1-2 (0x3b) command is also supported by the Extended SPI Protocol.
	 * We can change the mode safely as we write into a volatile register.
	 */
	ret = micron_set_extended_spi_protocol(nor);
	if (ret)
		return ret;

	/*
	 * Use the Fast Read Dual Output 1-1-2 command.
	 * Force the number of dummy cycles to 8 and disable the Continuous Read
	 * mode to prevent some drivers from using it by mistake (m25p80).
	 * We can change these settings safely as we write into a volatile
	 * register.
	 */
	nor->read_proto = SPI_PROTO_1_1_2;
	nor->read_opcode = SPINOR_OP_READ_1_1_2;
	return micron_set_dummy_cycles(nor, 8);
}

static int micron_set_single_mode(struct spi_nor *nor)
{
	u8 read_dummy;
	int ret;

	/*
	 * Exit Dual or Quad mode if not done yet.
	 * We can change the mode safely as we write into a volatile register.
	 */
	ret = micron_set_extended_spi_protocol(nor);
	if (ret)
		return ret;

	/*
	 * Force the number of dummy cycles to 8 (Fast Read only, Read doesn't
	 * care) and disable the Continuous Read mode to prevent some drivers
	 * from using it by mistake (m25p80).
	 * We can change these settings safely as we write into a volatile
	 * register.
	 */
	switch (nor->read_opcode) {
	case SPINOR_OP_READ:
	case SPINOR_OP_READ4:
		read_dummy = 0;
		break;

	default:
		read_dummy = 8;
		break;
	}

	nor->read_proto = SPI_PROTO_1_1_1;
	return micron_set_dummy_cycles(nor, read_dummy);
}

static int set_quad_mode(struct spi_nor *nor, struct flash_info *info)
{
	switch (JEDEC_MFR(info)) {
	case CFI_MFR_MACRONIX:
		return macronix_set_quad_mode(nor);

	case CFI_MFR_ST:
		return micron_set_quad_mode(nor);

	case CFI_MFR_AMD:
		return spansion_set_quad_mode(nor);

	default:
		break;
	}

	return -EINVAL;
}

static int set_dual_mode(struct spi_nor *nor, const struct flash_info *info)
{
	switch (JEDEC_MFR(info)) {
	case CFI_MFR_MACRONIX:
		return macronix_set_dual_mode(nor);

	case CFI_MFR_ST:
		return micron_set_dual_mode(nor);

	case CFI_MFR_AMD:
		return spansion_set_dual_output(nor);

	default:
		break;
	}

	return -EINVAL;
}

static int set_single_mode(struct spi_nor *nor, const struct flash_info *info)
{
	switch (JEDEC_MFR(info)) {
	case CFI_MFR_MACRONIX:
		return macronix_set_single_mode(nor);

	case CFI_MFR_ST:
		return micron_set_single_mode(nor);

	case CFI_MFR_AMD:
		return spansion_set_single(nor);

	default:
		break;
	}

	return -EINVAL;
}

static int spi_nor_check(struct spi_nor *nor)
{
	if (!nor->dev || !nor->read || !nor->write ||
		!nor->read_reg || !nor->write_reg || !nor->erase) {
		pr_err("spi-nor: please fill all the necessary fields!\n");
		return -EINVAL;
	}

	return 0;
}

int spi_nor_scan(struct spi_nor *nor, const char *name, enum read_mode mode)
{
	const struct spi_device_id	*id = NULL;
	struct flash_info		*info;
	struct device *dev = nor->dev;
	struct mtd_info *mtd = nor->mtd;
	struct device_node *np = dev->of_node;
	int ret;
	int i;

	ret = spi_nor_check(nor);
	if (ret)
		return ret;

	/* Reset SPI protocol for all commands */
	nor->erase_proto = SPI_PROTO_1_1_1;
	nor->read_proto = SPI_PROTO_1_1_1;
	nor->write_proto = SPI_PROTO_1_1_1;
	nor->reg_proto = SPI_PROTO_1_1_1;

	/* Try to auto-detect if chip name wasn't specified */
	if (!name)
		id = spi_nor_read_id(nor, mode);
	else
		id = spi_nor_match_id(name);
	if (IS_ERR_OR_NULL(id))
		return -ENOENT;

	info = (void *)id->driver_data;

	/*
	 * If caller has specified name of flash model that can normally be
	 * detected using JEDEC, let's verify it.
	 */
	if (name && info->id_len) {
		const struct spi_device_id *jid;

		jid = spi_nor_read_id(nor, mode);
		if (IS_ERR(jid)) {
			return PTR_ERR(jid);
		} else if (jid != id) {
			/*
			 * JEDEC knows better, so overwrite platform ID. We
			 * can't trust partitions any longer, but we'll let
			 * mtd apply them anyway, since some partitions may be
			 * marked read-only, and we don't want to lose that
			 * information, even if it's not 100% accurate.
			 */
			dev_warn(dev, "found %s, expected %s\n",
				 jid->name, id->name);
			id = jid;
			info = (void *)jid->driver_data;
		}
	}

	mutex_init(&nor->lock);

	/*
	 * Atmel, SST and Intel/Numonyx serial nor tend to power
	 * up with the software protection bits set
	 */

	if (JEDEC_MFR(info) == CFI_MFR_ATMEL ||
	    JEDEC_MFR(info) == CFI_MFR_INTEL ||
	    JEDEC_MFR(info) == CFI_MFR_SST) {
		write_enable(nor);
		write_sr(nor, 0);
	}

	if (!mtd->name)
		mtd->name = dev_name(dev);
	mtd->type = MTD_NORFLASH;
	mtd->writesize = 1;
	mtd->flags = MTD_CAP_NORFLASH;
	mtd->size = info->sector_size * info->n_sectors;
	mtd->_erase = spi_nor_erase;
	mtd->_read = spi_nor_read;

	/* nor protection support for STmicro chips */
	if (JEDEC_MFR(info) == CFI_MFR_ST) {
		nor->flash_lock = stm_lock;
		nor->flash_unlock = stm_unlock;
	}

	if (nor->flash_lock && nor->flash_unlock) {
		mtd->_lock = spi_nor_lock;
		mtd->_unlock = spi_nor_unlock;
	}

	/* sst nor chips use AAI word program */
	if (info->flags & SST_WRITE)
		mtd->_write = sst_write;
	else
		mtd->_write = spi_nor_write;

	if (info->flags & USE_FSR)
		nor->flags |= SNOR_F_USE_FSR;

#ifdef CONFIG_MTD_SPI_NOR_USE_4K_SECTORS
	/* prefer "small sector" erase if possible */
	if (info->flags & SECT_4K) {
		nor->erase_opcode = SPINOR_OP_BE_4K;
		mtd->erasesize = 4096;
	} else if (info->flags & SECT_4K_PMC) {
		nor->erase_opcode = SPINOR_OP_BE_4K_PMC;
		mtd->erasesize = 4096;
	} else
#endif
	{
		nor->erase_opcode = SPINOR_OP_SE;
		mtd->erasesize = info->sector_size;
	}

	if (info->flags & SPI_NOR_NO_ERASE)
		mtd->flags |= MTD_NO_ERASE;

	mtd->dev.parent = dev;
	nor->page_size = info->page_size;
	mtd->writebufsize = nor->page_size;

	if (np) {
		/* If we were instantiated by DT, use it */
		if (of_property_read_bool(np, "m25p,fast-read"))
			nor->flash_read = SPI_NOR_FAST;
		else
			nor->flash_read = SPI_NOR_NORMAL;
	} else {
		/* If we weren't instantiated by DT, default to fast-read */
		nor->flash_read = SPI_NOR_FAST;
	}

	/* Some devices cannot do fast-read, no matter what DT tells us */
	if (info->flags & SPI_NOR_NO_FR)
		nor->flash_read = SPI_NOR_NORMAL;

	/* Default commands and number of dummy cycles */
	nor->program_opcode = SPINOR_OP_PP;
	if (nor->flash_read == SPI_NOR_NORMAL) {
		nor->read_opcode = SPINOR_OP_READ;
		nor->read_dummy = 0;
	} else {
		nor->read_opcode = SPINOR_OP_READ_FAST;
		nor->read_dummy = 8;
	}

	/*
	 * Quad/Dual-read mode takes precedence over fast/normal. The opcodes,
	 * the protocols and the number of dummy cycles are updated depending
	 * on the manufacturer. The read opcode and protocol should be updated
	 * by the relevant function when entering Quad or Dual mode.
	 */
	if (mode == SPI_NOR_QUAD && info->flags & SPI_NOR_QUAD_READ) {
		ret = set_quad_mode(nor, info);
		if (ret) {
			dev_err(dev, "quad mode not supported\n");
			return ret;
		}
		nor->flash_read = SPI_NOR_QUAD;
	} else if (mode == SPI_NOR_DUAL && info->flags & SPI_NOR_DUAL_READ) {
		ret = set_dual_mode(nor, info);
		if (ret) {
			dev_err(dev, "dual mode not supported\n");
			return ret;
		}
		nor->flash_read = SPI_NOR_DUAL;
	} else if (info->flags & (SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ)) {
		/* We may need to leave a Quad or Dual mode */
		ret = set_single_mode(nor, info);
		if (ret) {
			dev_err(dev, "failed to switch back to single mode\n");
			return ret;
		}
	}

	if (info->addr_width)
		nor->addr_width = info->addr_width;
	else if (mtd->size > 0x1000000) {
		/* enable 4-byte addressing if the device exceeds 16MiB */
		nor->addr_width = 4;
		if (JEDEC_MFR(info) == CFI_MFR_AMD) {
			/* Dedicated 4-byte command set */
			switch (nor->flash_read) {
			case SPI_NOR_QUAD:
				nor->read_opcode = SPINOR_OP_READ4_1_1_4;
				break;
			case SPI_NOR_DUAL:
				nor->read_opcode = SPINOR_OP_READ4_1_1_2;
				break;
			case SPI_NOR_FAST:
				nor->read_opcode = SPINOR_OP_READ4_FAST;
				break;
			case SPI_NOR_NORMAL:
				nor->read_opcode = SPINOR_OP_READ4;
				break;
			}
			nor->program_opcode = SPINOR_OP_PP_4B;
			/* No small sector erase for 4-byte command set */
			nor->erase_opcode = SPINOR_OP_SE_4B;
			mtd->erasesize = info->sector_size;
		} else
			set_4byte(nor, info, 1);
	} else {
		nor->addr_width = 3;
	}

	dev_info(dev, "%s (%lld Kbytes)\n", id->name,
			(long long)mtd->size >> 10);

	dev_dbg(dev,
		"mtd .name = %s, .size = 0x%llx (%lldMiB), "
		".erasesize = 0x%.8x (%uKiB) .numeraseregions = %d\n",
		mtd->name, (long long)mtd->size, (long long)(mtd->size >> 20),
		mtd->erasesize, mtd->erasesize / 1024, mtd->numeraseregions);

	if (mtd->numeraseregions)
		for (i = 0; i < mtd->numeraseregions; i++)
			dev_dbg(dev,
				"mtd.eraseregions[%d] = { .offset = 0x%llx, "
				".erasesize = 0x%.8x (%uKiB), "
				".numblocks = %d }\n",
				i, (long long)mtd->eraseregions[i].offset,
				mtd->eraseregions[i].erasesize,
				mtd->eraseregions[i].erasesize / 1024,
				mtd->eraseregions[i].numblocks);
	return 0;
}
EXPORT_SYMBOL_GPL(spi_nor_scan);

static const struct spi_device_id *spi_nor_match_id(const char *name)
{
	const struct spi_device_id *id = spi_nor_ids;

	while (id->name[0]) {
		if (!strcmp(name, id->name))
			return id;
		id++;
	}
	return NULL;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huang Shijie <shijie8@gmail.com>");
MODULE_AUTHOR("Mike Lavender");
MODULE_DESCRIPTION("framework for SPI NOR");
