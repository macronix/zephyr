/*
 * Copyright (c) 2025-2026 Macronix International Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_mxic_uefc_spi

#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mxic_uefc);

#include "spi_context.h"

/*
 * PIO (register push/pull) mode only. DMA and memory-mapped (XIP) transfer
 * modes are supported by this host controller but are not implemented here.
 */

#define CHIP_SELECT_COUNT_DEFAULT	3u

/* Host Controller Register */
#define HC_CTRL				0x00
#define HC_CTRL_SIO_SHIFTER(x)			(((x) & 0x3) << 23)
#define HC_CTRL_SIO_SHIFTER_MASK		HC_CTRL_SIO_SHIFTER(0x3)
#define HC_CTRL_CH_SEL_B			BIT(11)
#define HC_CTRL_CH_SEL_A			0
#define HC_CTRL_CH_MASK				BIT(11)
#define HC_CTRL_LUN_SEL(x)			(((x) & 0x7) << 8)
#define HC_CTRL_LUN_MASK			HC_CTRL_LUN_SEL(0x7)
#define HC_CTRL_PORT_SEL(x)			(((x) & 0xff) << 0)
#define HC_CTRL_PORT_MASK			HC_CTRL_PORT_SEL(0xff)
#define HC_CTRL_CH_LUN_PORT_MASK		(HC_CTRL_CH_MASK | HC_CTRL_LUN_MASK | HC_CTRL_PORT_MASK)
#define HC_CTRL_CH_LUN_PORT(ch, lun, port)	(HC_CTRL_CH_SEL_##ch | HC_CTRL_LUN_SEL(lun) | \
						 HC_CTRL_PORT_SEL(port))

/* Normal Interrupt Status Register */
#define INT_STS				0x04
#define INT_STS_AC_RDY				BIT(28)
#define INT_STS_ERR_INT				BIT(15)
#define INT_STS_DMA_TFR_CMPLT			BIT(7)
#define INT_STS_DMA_INT				BIT(6)
#define INT_STS_ALL_CLR				(INT_STS_AC_RDY | INT_STS_ERR_INT | \
						 INT_STS_DMA_TFR_CMPLT | INT_STS_DMA_INT)

/* Error Interrupt Status Register */
#define ERR_INT_STS			0x08
#define ERR_INT_STS_ECC				BIT(19)
#define ERR_INT_STS_PREAM			BIT(18)
#define ERR_INT_STS_CRC				BIT(17)
#define ERR_INT_STS_AC				BIT(16)
#define ERR_INT_STS_ADMA			BIT(9)
#define ERR_INT_STS_ALL_CLR			(ERR_INT_STS_ECC | ERR_INT_STS_PREAM | \
						 ERR_INT_STS_CRC | ERR_INT_STS_AC | ERR_INT_STS_ADMA)

/* Normal Interrupt Status Enable Register */
#define INT_STS_EN			0x0C
#define INT_STS_EN_AC_RDY			BIT(28)
#define INT_STS_EN_ERR_INT			BIT(15)
#define INT_STS_EN_DMA_TFR_CMPLT		BIT(7)
#define INT_STS_EN_DMA_INT			BIT(3)
#define INT_STS_EN_ALL_EN			(INT_STS_EN_AC_RDY | INT_STS_EN_ERR_INT | \
						 INT_STS_EN_DMA_TFR_CMPLT | INT_STS_EN_DMA_INT)

/* Error Interrupt Status Enable Register */
#define ERR_INT_STS_EN			0x10
#define ERR_INT_STS_EN_ECC			BIT(19)
#define ERR_INT_STS_EN_PREAM			BIT(18)
#define ERR_INT_STS_EN_CRC			BIT(17)
#define ERR_INT_STS_EN_AC			BIT(16)
#define ERR_INT_STS_EN_ADMA			BIT(9)
#define ERR_INT_STS_EN_ALL_EN			(ERR_INT_STS_EN_ECC | ERR_INT_STS_EN_PREAM | \
						 ERR_INT_STS_EN_CRC | ERR_INT_STS_EN_AC | \
						 ERR_INT_STS_EN_ADMA)

/* Normal Interrupt Signal Enable Register */
#define INT_STS_SIG_EN			0x14
#define INT_STS_SIG_EN_AC_RDY			BIT(28)
#define INT_STS_SIG_EN_ERR_INT			BIT(15)
#define INT_STS_SIG_EN_DMA_TFR_CMPLT		BIT(7)
#define INT_STS_SIG_EN_DMA_INT			BIT(3)
#define INT_STS_SIG_EN_ALL_EN			(INT_STS_SIG_EN_AC_RDY | INT_STS_SIG_EN_ERR_INT | \
						 INT_STS_SIG_EN_DMA_TFR_CMPLT | \
						 INT_STS_SIG_EN_DMA_INT)

/* Error Interrupt Signal Enable Register */
#define ERR_INT_STS_SIG_EN		0x18
#define ERR_INT_STS_SIG_EN_ECC			BIT(19)
#define ERR_INT_STS_SIG_EN_PREAM		BIT(18)
#define ERR_INT_STS_SIG_EN_CRC			BIT(17)
#define ERR_INT_STS_SIG_EN_AC			BIT(16)
#define ERR_INT_STS_SIG_EN_ADMA			BIT(9)
#define ERR_INT_STS_SIG_EN_ALL_EN		(ERR_INT_STS_SIG_EN_ECC | ERR_INT_STS_SIG_EN_PREAM | \
						 ERR_INT_STS_SIG_EN_CRC | ERR_INT_STS_SIG_EN_AC | \
						 ERR_INT_STS_SIG_EN_ADMA)

/* Transfer Mode register: left at 0 (single-line, single data rate, no DMA). */
#define TFR_MODE			0x1C

/* Transfer Control Register */
#define TFR_CTRL			0x20
#define TFR_CTRL_DEV_DIS			BIT(18)
#define TFR_CTRL_IO_END				BIT(16)
#define TFR_CTRL_DEV_ACT			BIT(2)
#define TFR_CTRL_HC_ACT				BIT(1)
#define TFR_CTRL_IO_START			BIT(0)

/* Present State Register */
#define PRES_STS			0x24
#define PRES_STS_RX_NEMPT			BIT(18)
#define PRES_STS_TX_NFULL			BIT(17)

/* Mapping Base / Top Address Registers */
#define BASE_MAP_ADDR			0x38
#define TOP_MAP_ADDR			0xD0

/* Clock Control Register */
#define CLK_CTRL			0x4C
#define CLK_CTRL_RX_SS_B(x)			(((x) & 0x1F) << 21)
#define CLK_CTRL_RX_SS_A(x)			(((x) & 0x1F) << 16)

/* Capabilities Register */
#define CAP_1				0x58
#define CAP_1_CSB_NUM_MASK			0x1FF

/* Transmit Data 0~3 Register */
#define TXD_REG				0x70
#define TXD(x)					(TXD_REG + ((x) * 4))

/* Receive Data Register */
#define RXD_REG				0x80

/* Device Control Register */
#define DEV_CTRL			0xC0
#define DEV_CTRL_TYPE(x)			(((x) & 0x7) << 29)
#define DEV_CTRL_TYPE_MASK			DEV_CTRL_TYPE(0x7)
#define DEV_CTRL_TYPE_SPI			DEV_CTRL_TYPE(0)
#define DEV_CTRL_SCLK_SEL(x)			(((x) & 0xF) << 25)
#define DEV_CTRL_SCLK_SEL_MASK			DEV_CTRL_SCLK_SEL(0xF)
#define DEV_CTRL_SCLK_SEL_DIV(x)		DEV_CTRL_SCLK_SEL(((x) >> 1) - 1)
/* SCLK_SEL is 4 bits and encodes (div / 2 - 1), so the divider saturates at 32. */
#define DEV_CTRL_SCLK_DIV_MAX			32

/* Sample Point Adjust Register */
#define SAMPLE_ADJ			0xEC
#define SAMPLE_ADJ_DQS_IDLY_DOPI(x)		(((x) & 0xff) << 27)
#define SAMPLE_ADJ_POINT_SEL_DDR(x)		(((x) & 0x7) << 3)
#define SAMPLE_ADJ_POINT_SEL_SDR(x)		(((x) & 0x7) << 0)

/* SIO Input/Output Delay Registers */
#define SIO_IDLY_1			0xF0
#define SIO_IDLY_2			0xF4
#define SIO_ODLY_1			0xF8
#define SIO_ODLY_2			0xFC

#define UEFC_BASE_MAP_ADDR		0x60000000
#define UEFC_MAP_SIZE			0x00800000
#define UEFC_TOP_MAP_ADDR		(UEFC_BASE_MAP_ADDR + UEFC_MAP_SIZE)

#define MXIC_UEFC_POLL_RETRIES		1000000

struct mxic_uefc_config {
	DEVICE_MMIO_ROM;
	uint32_t clock_frequency;
};

struct mxic_uefc_data {
	struct spi_context ctx;
	uint8_t csb_num;
};

static inline uint32_t reg_read(const struct device *dev, uint32_t off)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + off);
}

static inline void reg_write(const struct device *dev, uint32_t off, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + off);
}

static inline void reg_update(const struct device *dev, uint32_t off, uint32_t mask, uint32_t val)
{
	reg_write(dev, off, val | (reg_read(dev, off) & ~mask));
}

/*
 * Busy-wait rather than k_usleep(): this is reached from spi_nand_init() at
 * POST_KERNEL, before the boot banner, and yielding there depends on the
 * timer interrupt already being live.  A FIFO slot frees up in well under a
 * microsecond at any supported SCLK, so spinning costs nothing.
 */
static int mxic_uefc_poll(const struct device *dev, uint32_t mask)
{
	uint32_t retries = MXIC_UEFC_POLL_RETRIES;

	while (retries--) {
		if (reg_read(dev, PRES_STS) & mask) {
			return 0;
		}
	}

	LOG_ERR("PRES_STS mask 0x%08x never asserted (PRES_STS=0x%08x)",
		mask, reg_read(dev, PRES_STS));

	return -ETIMEDOUT;
}

/*
 * Each TFR_CTRL action bit is written as a 1 and cleared by the controller
 * once it has been carried out.  Bound the wait: this runs at POST_KERNEL,
 * before the boot banner is printed, so spinning forever here leaves the
 * console completely silent with no indication of what went wrong.
 */
static int mxic_uefc_wait_tfr_ctrl(const struct device *dev, uint32_t bit)
{
	uint32_t retries = MXIC_UEFC_POLL_RETRIES;

	while (retries--) {
		if (!(reg_read(dev, TFR_CTRL) & bit)) {
			return 0;
		}
	}

	LOG_ERR("TFR_CTRL bit 0x%08x stuck (TFR_CTRL=0x%08x)", bit, reg_read(dev, TFR_CTRL));

	return -ETIMEDOUT;
}

static int mxic_uefc_cs_start(const struct device *dev)
{
	static const uint32_t seq[] = { TFR_CTRL_IO_START, TFR_CTRL_HC_ACT, TFR_CTRL_DEV_ACT };

	for (int i = 0; i < ARRAY_SIZE(seq); i++) {
		int ret;

		reg_write(dev, TFR_CTRL, seq[i]);
		ret = mxic_uefc_wait_tfr_ctrl(dev, seq[i]);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int mxic_uefc_cs_end(const struct device *dev)
{
	static const uint32_t seq[] = { TFR_CTRL_DEV_DIS, TFR_CTRL_IO_END };

	for (int i = 0; i < ARRAY_SIZE(seq); i++) {
		int ret;

		reg_write(dev, TFR_CTRL, seq[i]);
		ret = mxic_uefc_wait_tfr_ctrl(dev, seq[i]);
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * IO_END clears before the controller has actually finished releasing
	 * the bus, so back-to-back transfers can run into each other.  A
	 * PROGRAM EXECUTE issued too soon after its PROGRAM LOAD never reaches
	 * the device: the chip reports PROGRAM_FAIL and the page stays erased.
	 *
	 * Found with upstream's spi_nand driver, whose command path is leaner
	 * than this tree's and so leaves less time between transfers.  This
	 * driver's own spi_nand does more work per command and mostly gets
	 * away with it -- but not always, which is the likeliest explanation
	 * for the intermittent program failures seen during bring-up.
	 */
	k_busy_wait(2);

	return 0;
}

static int mxic_uefc_configure(const struct device *dev, const struct spi_config *config)
{
	struct mxic_uefc_data *data = dev->data;
	const struct mxic_uefc_config *cfg = dev->config;
	uint32_t div = 2;

	if (SPI_WORD_SIZE_GET(config->operation) != 8) {
		LOG_ERR("Word size must be 8");
		return -ENOTSUP;
	}

	if ((config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		LOG_ERR("Only single-line mode is supported");
		return -ENOTSUP;
	}

	if (config->operation & (SPI_MODE_CPOL | SPI_MODE_CPHA)) {
		LOG_ERR("Only SPI mode 0 is supported");
		return -ENOTSUP;
	}

	if (config->operation & SPI_TRANSFER_LSB) {
		LOG_ERR("LSB first is not supported");
		return -ENOTSUP;
	}

	if (config->operation & SPI_OP_MODE_SLAVE) {
		LOG_ERR("Slave mode is not supported");
		return -ENOTSUP;
	}

	if (config->operation & SPI_MODE_LOOP) {
		LOG_ERR("Loopback is not supported");
		return -ENOTSUP;
	}

	if (spi_cs_is_gpio(config)) {
		LOG_ERR("GPIO chip-select is not supported, CS is selected via HC_CTRL");
		return -ENOTSUP;
	}

	if (config->slave >= data->csb_num) {
		LOG_ERR("Unsupported chip-select %u (max %u)", config->slave, data->csb_num);
		return -ENOTSUP;
	}

	reg_update(dev, HC_CTRL, HC_CTRL_CH_LUN_PORT_MASK,
		   HC_CTRL_CH_LUN_PORT(A, 0, config->slave));

	if (config->frequency != 0 && cfg->clock_frequency > config->frequency) {
		div = cfg->clock_frequency / config->frequency;
		div += div & 1;
		div = CLAMP(div, 2, DEV_CTRL_SCLK_DIV_MAX);
	}

	reg_update(dev, DEV_CTRL, DEV_CTRL_TYPE_MASK | DEV_CTRL_SCLK_SEL_MASK,
		   DEV_CTRL_TYPE_SPI | DEV_CTRL_SCLK_SEL_DIV(div));

	/*
	 * spi_context_release() dereferences ctx->config unconditionally, so
	 * this must be recorded or the very first release reads through a NULL
	 * pointer and may skip k_sem_give(), wedging every later transfer.
	 */
	data->ctx.config = config;

	return 0;
}

static int mxic_uefc_transceive(const struct device *dev, const struct spi_config *config,
				 const struct spi_buf_set *tx_bufs,
				 const struct spi_buf_set *rx_bufs)
{
	struct mxic_uefc_data *data = dev->data;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, config);

	ret = mxic_uefc_configure(dev, config);
	if (ret < 0) {
		spi_context_release(&data->ctx, ret);
		return ret;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	ret = mxic_uefc_cs_start(dev);
	if (ret < 0) {
		spi_context_release(&data->ctx, ret);
		return ret;
	}

	while (spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx)) {
		uint32_t nbytes = MIN(spi_context_max_continuous_chunk(&data->ctx), 4);
		uint32_t tx_word = 0xffffffff;
		uint32_t rx_word;


		if (spi_context_tx_buf_on(&data->ctx)) {
			memcpy(&tx_word, data->ctx.tx_buf, nbytes);
		}

		ret = mxic_uefc_poll(dev, PRES_STS_TX_NFULL);
		if (ret < 0) {
			break;
		}
		reg_write(dev, TXD(nbytes % 4), tx_word);

		ret = mxic_uefc_poll(dev, PRES_STS_RX_NEMPT);
		if (ret < 0) {
			break;
		}
		rx_word = reg_read(dev, RXD_REG);

		if (spi_context_rx_buf_on(&data->ctx)) {
			memcpy(data->ctx.rx_buf, &rx_word, nbytes);
		}

		spi_context_update_tx(&data->ctx, 1, nbytes);
		spi_context_update_rx(&data->ctx, 1, nbytes);
	}

	{
		int end_ret = mxic_uefc_cs_end(dev);

		if (ret == 0) {
			ret = end_ret;
		}
	}

	spi_context_release(&data->ctx, ret);

	return ret;
}

static int mxic_uefc_release(const struct device *dev, const struct spi_config *config)
{
	struct mxic_uefc_data *data = dev->data;

	ARG_UNUSED(config);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static int mxic_uefc_init(const struct device *dev)
{
	struct mxic_uefc_data *data = dev->data;
	uint32_t csb_num;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	reg_write(dev, BASE_MAP_ADDR, UEFC_BASE_MAP_ADDR);
	reg_write(dev, TOP_MAP_ADDR, UEFC_TOP_MAP_ADDR);

	reg_update(dev, HC_CTRL, HC_CTRL_SIO_SHIFTER_MASK, HC_CTRL_SIO_SHIFTER(3));
	reg_write(dev, CLK_CTRL, CLK_CTRL_RX_SS_A(1) | CLK_CTRL_RX_SS_B(1));

	reg_write(dev, INT_STS, INT_STS_ALL_CLR);
	reg_write(dev, INT_STS_EN, INT_STS_EN_ALL_EN);
	reg_write(dev, INT_STS_SIG_EN, INT_STS_SIG_EN_ALL_EN);

	reg_write(dev, ERR_INT_STS, ERR_INT_STS_ALL_CLR);
	reg_write(dev, ERR_INT_STS_EN, ERR_INT_STS_EN_ALL_EN);
	reg_write(dev, ERR_INT_STS_SIG_EN, ERR_INT_STS_SIG_EN_ALL_EN);

	reg_write(dev, SAMPLE_ADJ, SAMPLE_ADJ_DQS_IDLY_DOPI(29) | SAMPLE_ADJ_POINT_SEL_DDR(0) |
				   SAMPLE_ADJ_POINT_SEL_SDR(2));
	reg_write(dev, SIO_IDLY_1, 0);
	reg_write(dev, SIO_IDLY_2, 0);
	reg_write(dev, SIO_ODLY_1, 0);
	reg_write(dev, SIO_ODLY_2, 0);

	/* Single-line, single data rate, PIO mode: no per-transfer bus-width setup needed. */
	reg_write(dev, TFR_MODE, 0);

	csb_num = (reg_read(dev, CAP_1) & CAP_1_CSB_NUM_MASK);
	data->csb_num = (csb_num != 0 && csb_num <= UINT8_MAX) ? csb_num : CHIP_SELECT_COUNT_DEFAULT;

	LOG_INF("UEFC @ 0x%lx: CAP_1=0x%08x HC_CTRL=0x%08x DEV_CTRL=0x%08x, %u CS",
		(unsigned long)DEVICE_MMIO_GET(dev), reg_read(dev, CAP_1),
		reg_read(dev, HC_CTRL), reg_read(dev, DEV_CTRL), data->csb_num);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, mxic_uefc_api) = {
	.transceive = mxic_uefc_transceive,
	.release = mxic_uefc_release,
};

#define MXIC_UEFC_INIT(inst)								\
	static struct mxic_uefc_data mxic_uefc_data_##inst = {			\
		SPI_CONTEXT_INIT_LOCK(mxic_uefc_data_##inst, ctx),			\
		SPI_CONTEXT_INIT_SYNC(mxic_uefc_data_##inst, ctx),			\
	};										\
	static const struct mxic_uefc_config mxic_uefc_config_##inst = {		\
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),				\
		.clock_frequency = DT_INST_PROP_OR(inst, clock_frequency, 0),		\
	};										\
	DEVICE_DT_INST_DEFINE(inst, mxic_uefc_init, NULL,				\
			      &mxic_uefc_data_##inst, &mxic_uefc_config_##inst,	\
			      POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,			\
			      &mxic_uefc_api);

DT_INST_FOREACH_STATUS_OKAY(MXIC_UEFC_INIT)
