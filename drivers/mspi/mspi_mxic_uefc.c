/*
 * Copyright (c) 2024, Ambiq Micro Inc. <www.ambiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_mspi_controller

#include "mspi_mxic_uefc.h"
LOG_MODULE_REGISTER(xlnx_mspi_controller);

static uint32_t mxic_uefc_conf(const struct device *dev);
static int mxic_uefc_hc_setup(const struct device *dev);
static int mxic_uefc_poll_hc_reg(const struct device *dev, uint32_t reg, uint32_t mask);
static int mxic_uefc_io_mode_xfer(const struct device *dev, void *tx, void *rx, uint32_t len,
				  uint8_t is_data);
static int mxic_uefc_init(const struct device *dev);
static void mxic_uefc_cs_start(const struct device *dev);
static int mspi_mxic_config(const struct mspi_dt_spec *spec);

struct mspi_mxic_config {
	DEVICE_MMIO_ROM;
};


struct mspi_mxic_data {
	DEVICE_MMIO_RAM;

	struct mspi_dev_id *dev_id;
	struct k_mutex lock;

#if defined(CONFIG_MSPI_XIP)
	uint32_t xip_freq;
	struct xip_params xip_params_stored;
	struct xip_params xip_params_active;
	uint16_t xip_enabled;
	enum mspi_cpp_mode xip_cpp;
#endif

	struct mspi_dev_cfg dev_cfg;
	struct mspi_xip_cfg xip_cfg;
	struct mspi_scramble_cfg scramble_cfg;
	uint8_t data_buswidth;
	bool data_dtr;

#if defined(CONFIG_MSPI_XIP)
struct xip_params {
	uint32_t read_cmd;
	uint32_t write_cmd;
	uint16_t rx_dummy;
	uint16_t tx_dummy;
	uint8_t cmd_length;
	uint8_t addr_length;
	enum mspi_data_rate data_rate;
	enum mspi_io_mode io_mode;
};

struct xip_ctrl {
	uint32_t read;
	uint32_t write;
};
#endif
	mspi_callback_handler_t cbs[MSPI_BUS_EVENT_MAX];
	struct mspi_callback_context *cb_ctxs[MSPI_BUS_EVENT_MAX];
	struct mspi_context ctx;
};

static int mxic_uefc_init(const struct device *dev)
{
	int ret = 0;
	const struct mspi_mxic_config *cfg = dev->config;

	struct mspi_mxic_data *data = dev->data;

	uint32_t uefc_version = 0;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	uefc_version = MXIC_RD32(reg_base + INT_STS_SIG_EN);
	MXIC_WR32(UEFC_BASE_MAP_ADDR, reg_base + BASE_MAP_ADDR);
	MXIC_WR32(UEFC_TOP_MAP_ADDR, reg_base + TOP_MAP_ADDR);
	printf("***[%s], [%s], [%04d],uefc_version is %x\r\n", __FILE__, __func__, __LINE__,
	       uefc_version);

	UPDATE_WRITE(HC_CTRL_CH_LUN_PORT_MASK, UEFC_CH_LUN_PORT, reg_base + HC_CTRL);
	UPDATE_WRITE(DEV_CTRL_TYPE_MASK | DEV_CTRL_SCLK_SEL_MASK,
		     DEV_CTRL_TYPE_SPI | DEV_CTRL_SCLK_SEL_DIV(4), reg_base + DEV_CTRL);

	uefc_version = MXIC_RD32(reg_base + HC_VER);

	UPDATE_WRITE(HC_CTRL_SIO_SHIFTER(3), HC_CTRL_SIO_SHIFTER(3), reg_base + HC_CTRL);
	printf("***[%s], [%s], [%04d],uefc_version is %x\r\n", __FILE__, __func__, __LINE__,
	       uefc_version);
	MXIC_WR32(CLK_CTRL_RX_SS_A(1) | CLK_CTRL_RX_SS_B(1), reg_base + CLK_CTRL);

	MXIC_WR32(INT_STS_ALL_CLR, reg_base + INT_STS);
	MXIC_WR32(INT_STS_EN_ALL_EN, reg_base + INT_STS_EN);
	MXIC_WR32(INT_STS_SIG_EN_ALL_EN, reg_base + INT_STS_SIG_EN);

	MXIC_WR32(ERR_INT_STS_ALL_CLR, reg_base + ERR_INT_STS);
	MXIC_WR32(ERR_INT_STS_EN_ALL_EN, reg_base + ERR_INT_STS_EN);
	MXIC_WR32(ERR_INT_STS_SIG_EN_ALL_EN, reg_base + ERR_INT_STS_SIG_EN);

	MXIC_WR32(INT_STS_DMA | INT_STS_EN_DMA_TFR_CMPLT, reg_base + INT_STS_EN);

	MXIC_WR32(SAMPLE_ADJ_DQS_IDLY_DOPI(0) | SAMPLE_ADJ_POINT_SEL_DDR(0) |
			  SAMPLE_ADJ_POINT_SEL_SDR(1),
		  reg_base + SAMPLE_ADJ);
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);
	MXIC_WR32(0, reg_base + SIO_IDLY_1);
	MXIC_WR32(0, reg_base + SIO_IDLY_2);
	MXIC_WR32(0, reg_base + SIO_ODLY_1);
	MXIC_WR32(0, reg_base + SIO_ODLY_2);

	//  k_sem_init(&data->sync_sem, 0, 1);

	return 0;
}

static int mxic_uefc_poll_hc_reg(const struct device *dev, uint32_t reg, uint32_t mask)
{
	uint32_t val, n = 10000;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	do {
		val = MXIC_RD32(reg_base + reg) & mask;
		n--;
		k_usleep(1);
	} while (!val && n);
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	if (!val) {
		printf("TIMEOUT! reg(%02Xh) & mask(%08Xh): val(%08Xh)\r\n", reg, mask, val);
		return -1;
	}
	return 0;
}

static void mxic_uefc_cs_start(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	/* Enable IO Mode */
	MXIC_WR32(TFR_CTRL_IO_START, reg_base + TFR_CTRL);
	while (TFR_CTRL_IO_START & MXIC_RD32(reg_base + TFR_CTRL)) {
		;
	}

	/* Enable host controller, reset counter */
	MXIC_WR32(TFR_CTRL_HC_ACT, reg_base + TFR_CTRL);
	while (TFR_CTRL_HC_ACT & MXIC_RD32(reg_base + TFR_CTRL)) {
		;
	}

	/* Assert CS */
	MXIC_WR32(TFR_CTRL_DEV_ACT, reg_base + TFR_CTRL);
	while (TFR_CTRL_DEV_ACT & MXIC_RD32(reg_base + TFR_CTRL)) {
		;
	}
}

static void mxic_uefc_cs_end(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	/* De-assert CS */
	MXIC_WR32(TFR_CTRL_DEV_DIS, reg_base + TFR_CTRL);
	while (TFR_CTRL_DEV_DIS & MXIC_RD32(reg_base + TFR_CTRL)) {
		;
	}

	/* Disable IO Mode */
	MXIC_WR32(TFR_CTRL_IO_END, reg_base + TFR_CTRL);
	while (TFR_CTRL_IO_END & MXIC_RD32(reg_base + TFR_CTRL)) {
		;
	}
}

static inline void mxic_uefc_err_dessert_cs(const struct device *dev)
{
	mxic_uefc_cs_end(dev);
}

static int mxic_uefc_io_mode_xfer(const struct device *dev, void *tx, void *rx, uint32_t len,
				  uint8_t is_data)
{
	uint32_t nbytes, tmp = 0, ofs = 0;
	struct mspi_mxic_data *data_1 = dev->data;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	printf("***[%s], [%s], [%04d], reg_base is %x\r\n", __FILE__, __func__, __LINE__, reg_base);

	uint8_t data_octal_dtr = is_data && data_1->data_dtr && (8 == data_1->data_buswidth);

	while (ofs < len) {
		int ret;

		nbytes = len - ofs;
		uint32_t data = 0xffffffff;

		if (nbytes > 4) {
			nbytes = 4;
		}

		if (tx) {
			memcpy(&data, tx + ofs, nbytes);
			printf("tx data: %08X\r\n", data);
		}

		if (data_octal_dtr && (nbytes % 2)) {
			tmp = nbytes;
			nbytes++;
		}
		printf("***[%s], [%s], [%04d], reg_base is %x\r\n", __FILE__, __func__, __LINE__,
		       reg_base);

		ret = mxic_uefc_poll_hc_reg(dev, PRES_STS, PRES_STS_TX_NFULL);
		if (EXIT_SUCCESS != ret) {
			return ret;
		}

		printf("***[%s], [%s], [%04d], data is %x\r\n", __FILE__, __func__, __LINE__, data);

		printf("***[%s], [%s], [%04d], TXD(nbytes  4) is %x \r\n", __FILE__, __func__,
		       __LINE__, TXD(nbytes % 4));

		mxic_wr32(data, (reg_base + TXD(nbytes % 4)));
		printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		ret = mxic_uefc_poll_hc_reg(dev, PRES_STS, PRES_STS_RX_NEMPT);
		if (EXIT_SUCCESS != ret) {
			return ret;
		}
		printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		data = MXIC_RD32(reg_base + RXD_REG);
		if (rx) {
			memcpy(rx + ofs, &data, tmp ? tmp : nbytes);
		}
		printf("rx data: %08X\r\n", data);
		ofs += nbytes;
	}

	return EXIT_SUCCESS;
}

static uint32_t mspi_mxic_set_line(enum mspi_io_mode io_mode, enum mspi_data_rate data_rate)
{
	struct mspi_mxic_data *data = dev->data;

	if (data_rate != MSPI_DATA_RATE_SINGLE) {
		// LOG_INST_ERR(cfg->log, "%u, incorrect data rate, only SDR is supported.",
		// __LINE__);
		return -EINVAL;
	}

	uint32_t cmd_bus = 0;
	uint32_t addr_bus = 0;
	uint32_t data_bus = 0;

	bool cmd_ddr = false;
	bool addr_ddr = false;
	bool data_ddr = false;

	switch (data_rate) {
	case MSPI_DATA_RATE_SINGLE:
		break;
	case MSPI_DATA_RATE_S_S_D:
		data_ddr = true;
		break;
	case MSPI_DATA_RATE_S_D_D:
		addr_ddr = true;
		data_ddr = true;
		break;
	case MSPI_DATA_RATE_DUAL:
		cmd_ddr = addr_ddr = data_ddr = true;
		break;
	default:
		break;
	}

	uint8_t cmd_lines = 1;
	uint8_t addr_lines = 1;
	uint8_t data_lines = 1;

	switch (io_mode) {
	case MSPI_IO_MODE_SINGLE:
		break;
	case MSPI_IO_MODE_DUAL:
	case MSPI_IO_MODE_DUAL_1_1_2:
		data_lines = 2;
		break;
	case MSPI_IO_MODE_DUAL_1_2_2:
		addr_lines = data_lines = 2;
		break;
	case MSPI_IO_MODE_QUAD:
	case MSPI_IO_MODE_QUAD_1_4_4:
		addr_lines = data_lines = 4;
		break;
	case MSPI_IO_MODE_QUAD_1_1_4:
		data_lines = 4;
		break;
	case MSPI_IO_MODE_OCTAL:
	case MSPI_IO_MODE_OCTAL_1_8_8:
		addr_lines = data_lines = 8;
		break;
	case MSPI_IO_MODE_OCTAL_1_1_8:
		data_lines = 8;
		break;
	default:
		break;
	}

	cmd_bus = cmd_lines == 1 ? 0 : cmd_lines == 2 ? 1 : cmd_lines == 4 ? 2 : 3;
	addr_bus = addr_lines == 1 ? 0 : addr_lines == 2 ? 1 : addr_lines == 4 ? 2 : 3;
	data_bus = data_lines == 1 ? 0 : data_lines == 2 ? 1 : data_lines == 4 ? 2 : 3;

printf ("***[%s], [%s], [%04d], dev_cfg->cmd_length is %x \r\n", __FILE__, __func__, __LINE__, dev_cfg->cmd_length);
printf ("***[%s], [%s], [%04d], dev_cfg->addr_length is %x \r\n", __FILE__, __func__, __LINE__, dev_cfg->addr_length);

	uint32_t conf = OP_CMD_BUSW(cmd_bus) | OP_CMD_DTR(cmd_ddr ? 1 : 0);
 		printf ("***[%s], [%s], [%04d], mspi_mxic_set_line conf is %x \r\n", __FILE__, __func__, __LINE__, conf);

	conf |= OP_ADDR_BUSW(addr_bus) |
		OP_ADDR_DTR(addr_ddr ? 1 : 0);
 		printf ("***[%s], [%s], [%04d], mspi_mxic_set_line conf is %x \r\n", __FILE__, __func__, __LINE__, conf);

	conf |= OP_DATA_BUSW(data_bus) | OP_DATA_DTR(data_ddr ? 1 : 0);
 		printf ("***[%s], [%s], [%04d], mspi_mxic_set_line conf is %x \r\n", __FILE__, __func__, __LINE__, conf);

	data->data_buswidth = data_lines;
	data->data_dtr = data_ddr;

	return conf;
}

static int mspi_mxic_dev_config(const struct device *dev, const struct mspi_dev_id *dev_id,
				const enum mspi_dev_cfg_mask param_mask,
				const struct mspi_dev_cfg *dev_cfg)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	int ret = 0;

	const struct mspi_mxic_config *cfg = dev->config;

	enum mspi_io_mode io_mode = dev_cfg->io_mode;
	enum mspi_data_rate data_rate = dev_cfg->data_rate;

	uint32_t conf = mspi_mxic_set_line(io_mode, data_rate);

	MXIC_WR32(conf, reg_base + TFR_MODE);

	data->dev_cfg = *dev_cfg;
	data->dev_id = (struct mspi_dev_id *)dev_id;

	return ret;
}

static int mspi_dma_transceive(const struct device *controller,
			       const struct mspi_xfer *xfer,
			       mspi_callback_handler_t cb,
			       struct mspi_callback_context *cb_ctx)
{
	const struct mspi_ambiq_config *cfg = controller->config;
	struct mspi_ambiq_data *data = controller->data;
	struct mspi_context *ctx = &data->ctx;
	am_hal_mspi_dma_transfer_t trans;
	int ret = 0;
	int cfg_flag = 0;

	ret = mspi_xfer_config(controller, xfer);
	if (ret) {
		goto dma_err;
	}

	MXIC_WR32(mxic_uefc_conf(xfer), reg_base + TFR_MODE);
		mxic_uefc_cs_start(dev);

	/* Set up command  */
	if (xfer->cmd_length) {
		ret = mxic_uefc_io_mode_xfer(dev, (uint8_t *)&xfer->packets->cmd, 0,
					     xfer->cmd_length, 0);
		printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	/* Set up address */
	if (xfer->addr_length) {
		uint32_t addr = swap32(xfer->packets->address,  xfer->addr_length);

		ret = mxic_uefc_io_mode_xfer(dev, (uint8_t *)&addr, 0,
					     xfer->addr_length, 0);

		printf("***[%s], [%s], [%04d], xfer->addr_length is %x\r\n", __FILE__, __func__, __LINE__, xfer->addr_length);

		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
		}
	}
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	uint32_t dummy_length = MSPI_TX == xfer->packets->dir ? xfer->tx_dummy : xfer->rx_dummy;

	/* Setup dummy: dummy's bus width and DTR are determined by the data */
	if (dummy_length) {
		uint32_t dummy_len =
			(dummy_length * (data->data_dtr + 1)) / (8 / (data->data_buswidth));
		printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		ret = mxic_uefc_io_mode_xfer(dev, 0, 0, dummy_len, 0);
		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	/* Set up read/write Data */
	if (xfer->packets->data_buf) {
		printf("***[%s], [%s], [%04d], xfer->packets->data_buf is %x\r\n", __FILE__,
		       __func__, __LINE__, xfer->packets->data_buf[0]);

		ret = mxic_uefc_io_mode_xfer(
			dev, MSPI_TX == xfer->packets->dir ? xfer->packets->data_buf : 0,
			MSPI_RX == xfer->packets->dir ? xfer->packets->data_buf : 0,
			xfer->packets->num_bytes, 1);

		printf("***[%s], [%s], [%04d], xfer->packets->data_buf is %x\r\n", __FILE__,
		       __func__, __LINE__, xfer->packets->data_buf[0]);

		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	if (xfer->pkts->data.len) {
		do {
			reg_int_sts = MXIC_RD32(INT_STS);

			if (INT_STS_DMA_INT & reg_int_sts) {
				MXIC_WR32(INT_STS_DMA_INT, INT_STS);
				MXIC_WR32(MXIC_RD32(SDMA_ADDR), SDMA_ADDR);
			}

		} while (!(INT_STS_DMA_TFR_CMPLT & reg_int_sts));
	}

	mxic_uefc_cs_end(xfer);

	return ret;
}

#if defined(CONFIG_MSPI_XIP)
static bool apply_xip_config(const struct mspi_dw_data *dev_data,
			      struct xip_ctrl *ctrl)
{
	enum mspi_io_mode io_mode = dev_data->xip_params_active.io_mode;
	enum mspi_data_rate data_rate = dev_data->xip_params_active.data_rate;
	uint16_t rx_dummy = dev_data->xip_params_active.rx_dummy;
	uint16_t tx_dummy = dev_data->xip_params_active.tx_dummy;
	

	uint32_t conf = mspi_mxic_set_line(io_mode, data_rate);
	ctrl->read |= conf;

	ctrl->read  |=  OP_DD_RD;

	ctrl->write |= conf;
	ctrl->read  |= OP_DMY_CNT(rx_dummy, data->data_dtr, data->data_buswidth);
	ctrl->write  |= OP_DMY_CNT(tx_dummy, data->data_dtr, data->data_buswidth);
	
	return true;
}

#endif /* defined(CONFIG_MSPI_XIP) */

#if defined(CONFIG_MSPI_XIP)
static int _api_xip_config(const struct device *dev,
			   const struct mspi_dev_id *dev_id,
			   const struct mspi_xip_cfg *cfg)
{
	struct mspi_dw_data *dev_data = dev->data;
	int rc;

	if (!cfg->enable) {
		MXIC_WR32(TFR_CTRL_IO_END, TFR_CTRL);

		dev_data->xip_enabled &= ~BIT(dev_id->dev_idx);
		return 0;
	}


	if (!dev_data->xip_enabled) {
		struct xip_params *params = &dev_data->xip_params_active;
		struct xip_ctrl ctrl = {0};

		*params = dev_data->xip_params_stored;

		uint8_t read_cmd = dev_data->xip_params_active.read_cmd;
		uint8_t write_cmd = dev_data->xip_params_active.write_cmd;
		
		MXIC_WR32(TFR_CTRL_IO_END, reg_base + TFR_CTRL);
		apply_xip_config();

		MXIC_WR32(ctrl->read, reg_base + MAP_RD_CTRL);
		MXIC_WR32(ctrl->write, reg_base + MAP_WR_CTRL);

		MXIC_WR32(read_cmd, MAP_CMD);
		MXIC_WR32(read_cmd  << 16 , MAP_CMD);
	} else if (dev_data->xip_params_active.read_cmd !=
		   dev_data->xip_params_stored.read_cmd ||
		   dev_data->xip_params_active.write_cmd !=
		   dev_data->xip_params_stored.write_cmd ||
		   dev_data->xip_params_active.cmd_length !=
		   dev_data->xip_params_stored.cmd_length ||
		   dev_data->xip_params_active.addr_length !=
		   dev_data->xip_params_stored.addr_length ||
		   dev_data->xip_params_active.rx_dummy !=
		   dev_data->xip_params_stored.rx_dummy ||
		   dev_data->xip_params_active.tx_dummy !=
		   dev_data->xip_params_stored.tx_dummy) {
		LOG_ERR("Conflict with configuration already used for XIP.");
		return -EINVAL;
	}

	dev_data->xip_enabled |= BIT(dev_id->dev_idx);

	return 0;
}

static int api_xip_config(const struct device *dev,
			  const struct mspi_dev_id *dev_id,
			  const struct mspi_xip_cfg *cfg)
{
	struct mspi_dw_data *dev_data = dev->data;
	int rc, rc2;

	if (cfg->enable && dev_id != dev_data->dev_id) {
		LOG_ERR("Controller is not configured for this device");
		return -EINVAL;
	}

	rc = pm_device_runtime_get(dev);
	if (rc < 0) {
		LOG_ERR("pm_device_runtime_get() failed: %d", rc);
		return rc;
	}

	(void)k_sem_take(&dev_data->ctx_lock, K_FOREVER);

	if (dev_data->suspended) {
		rc = -EFAULT;
	} else {
		rc = _api_xip_config(dev, dev_id, cfg);
	}

	k_sem_give(&dev_data->ctx_lock);

	rc2 = pm_device_runtime_put(dev);
	if (rc2 < 0) {
		LOG_ERR("pm_device_runtime_put() failed: %d", rc2);
		rc = (rc < 0 ? rc : rc2);
	}

	return rc;
}
#endif /* defined(CONFIG_MSPI_XIP) */

static int mspi_pio_prepare(const struct device *dev, struct mspi_xfer *xfer)
{
	const struct mspi_mxic_config *cfg = dev->config;

	struct mspi_mxic_data *data = dev->data;

	int ret = 0;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	uint32_t conf = MXIC_RD32(reg_base + TFR_MODE);

	uint32_t conf_1 = 0;

	printf ("***[%s], [%s], [%04d], mspi_pio_prepare conf is %x \r\n", __FILE__, __func__, __LINE__, conf);

	uint16_t dummy_len = DIR_IN == xfer->packets->dir ? xfer->rx_dummy : xfer->tx_dummy;

	conf &= ~(TFR_MODE_ADDR_CNT_MASK | TFR_MODE_CMD_CNT | TFR_MODE_DMY_MASK | OP_DD_RD);
	printf ("***[%s], [%s], [%04d], mspi_pio_prepare confi is %x \r\n", __FILE__, __func__, __LINE__, conf);


	printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	conf |= OP_CMD_CNT(xfer->cmd_length) | OP_ADDR_CNT(xfer->addr_length);
	printf ("***[%s], [%s], [%04d], mspi_pio_prepare confi is %x \r\n", __FILE__, __func__, __LINE__, conf);

	conf_1 = OP_DMY_CNT(dummy_len, data->data_dtr, data->data_buswidth);
	printf ("***[%s], [%s], [%04d], mspi_pio_prepare conf_1 is %x \r\n", __FILE__, __func__, __LINE__, conf_1);

	conf |= OP_DMY_CNT(dummy_len, data->data_dtr, data->data_buswidth);
	printf ("***[%s], [%s], [%04d], mspi_pio_prepare confi is %x \r\n", __FILE__, __func__, __LINE__, conf);

	conf |= (DIR_IN == xfer->packets->dir ? OP_DD_RD : 0);
	printf ("***[%s], [%s], [%04d], mspi_pio_prepare confi is %x \r\n", __FILE__, __func__, __LINE__, conf);

	MXIC_WR32(conf, reg_base + TFR_MODE);
	printf ("***[%s], [%s], [%04d], mspi_pio_prepare confi is %x \r\n", __FILE__, __func__, __LINE__, conf);

	return ret;
}

static int api_xip_config(const struct device *dev,
			  const struct mspi_dev_id *dev_id,
			  const struct mspi_xip_cfg *cfg)
{
	struct mspi_dw_data *dev_data = dev->data;

	/* End IO Mode */
	MXIC_WR32(TFR_CTRL_IO_END, reg_base + TFR_CTRL);

	return 0;
}

static int mspi_pio_transceive(const struct device *dev, const struct mspi_xfer *xfer,
			       mspi_callback_handler_t cb, struct mspi_callback_context *cb_ctx)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	struct mspi_context *ctx = &data->ctx;
	const struct mspi_xfer_packet *packet;
	uint32_t packet_idx;
	int ret = 0;

	mspi_pio_prepare(dev, xfer);

	mxic_uefc_cs_start(dev);

	/* Set up command  */
	if (xfer->cmd_length) {
		ret = mxic_uefc_io_mode_xfer(dev, (uint8_t *)&xfer->packets->cmd, 0,
					     xfer->cmd_length, 0);
		printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	/* Set up address */
	if (xfer->addr_length) {
		uint32_t addr = swap32(xfer->packets->address,  xfer->addr_length);

		ret = mxic_uefc_io_mode_xfer(dev, (uint8_t *)&addr, 0,
					     xfer->addr_length, 0);

		printf("***[%s], [%s], [%04d], xfer->addr_length is %x\r\n", __FILE__, __func__, __LINE__, xfer->addr_length);

		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
		}
	}
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	uint32_t dummy_length = MSPI_TX == xfer->packets->dir ? xfer->tx_dummy : xfer->rx_dummy;

	/* Setup dummy: dummy's bus width and DTR are determined by the data */
	if (dummy_length) {
		uint32_t dummy_len =
			(dummy_length * (data->data_dtr + 1)) / (8 / (data->data_buswidth));
		printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		ret = mxic_uefc_io_mode_xfer(dev, 0, 0, dummy_len, 0);
		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	/* Set up read/write Data */
	if (xfer->packets->data_buf) {
		printf("***[%s], [%s], [%04d], xfer->packets->data_buf is %x\r\n", __FILE__,
		       __func__, __LINE__, xfer->packets->data_buf[0]);

		ret = mxic_uefc_io_mode_xfer(
			dev, MSPI_TX == xfer->packets->dir ? xfer->packets->data_buf : 0,
			MSPI_RX == xfer->packets->dir ? xfer->packets->data_buf : 0,
			xfer->packets->num_bytes, 1);

		printf("***[%s], [%s], [%04d], xfer->packets->data_buf is %x\r\n", __FILE__,
		       __func__, __LINE__, xfer->packets->data_buf[0]);

		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	mxic_uefc_cs_end(dev);
	printf("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	return ret;
}

static int mspi_mxic_transceive(const struct device *dev, const struct mspi_dev_id *dev_id,
				const struct mspi_xfer *xfer)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	mspi_callback_handler_t cb = NULL;
	struct mspi_callback_context *cb_ctx = NULL;

	if (xfer->xfer_mode == MSPI_PIO) {
 		printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

		return mspi_pio_transceive(dev, xfer, cb, cb_ctx);
	} else if (xfer->xfer_mode == MSPI_DMA) {
		// return mspi_dma_transceive(dev, xfer, cb, cb_ctx);
	} else {
		return -EIO;
	}
}

static struct mspi_driver_api mspi_mxic_driver_api = {
	// .config = mspi_mxic_config,
	.dev_config = mspi_mxic_dev_config,
	//.get_channel_status    = mspi_mxic_get_channel_status,
	// .register_callback     = mspi_mxic_register_callback,
	.transceive = mspi_mxic_transceive,
};

static const struct mspi_mxic_config mspi_mxic_config_0 = {DEVICE_MMIO_ROM_INIT(DT_DRV_INST(0))};

static struct mspi_mxic_data mspi_mxic_data_0;

DEVICE_DT_INST_DEFINE(0, &mxic_uefc_init, NULL, &mspi_mxic_data_0, &mspi_mxic_config_0, POST_KERNEL,
		      CONFIG_MSPI_INIT_PRIORITY, &mspi_mxic_driver_api);
