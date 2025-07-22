/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT jedec_mspi_nor

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/init.h>

#include "flash_mspi_nor.h"
#include "flash_mspi_nor_quirks.h"
#define UEFC_BASE_MAP_ADDR 		0x60000000
#define UEFC_MAP_SIZE			0x00800000
LOG_MODULE_REGISTER(flash_mspi_nor, CONFIG_FLASH_LOG_LEVEL);

#define READ_ID_FORCE_SINGLE 1
#define IO_MODE 1
#define IO_MODE_DMA 0
#define DOPI_MODE 1
#define OCTA_MODE 1
#define XIP_MODE 0
#define DMA_MODE_RD 0
#define DMA_MODE_WR 0
#define TEST_MODE 1

void flash_mspi_command_set(const struct device *dev, const struct flash_mspi_nor_cmd *cmd)
{
	struct flash_mspi_nor_data *dev_data = dev->data;
	const struct flash_mspi_nor_config *dev_config = dev->config;

	memset(&dev_data->xfer, 0, sizeof(dev_data->xfer));
	memset(&dev_data->packet, 0, sizeof(dev_data->packet));

	dev_data->xfer.xfer_mode  = MSPI_PIO;
	dev_data->xfer.packets    = &dev_data->packet;
	dev_data->xfer.num_packet = 1;
	dev_data->xfer.timeout    = 10;

	dev_data->xfer.cmd_length = cmd->cmd_length;
	dev_data->xfer.addr_length = cmd->addr_length;
	dev_data->xfer.tx_dummy = (cmd->dir == MSPI_TX) ?
				  cmd->tx_dummy : dev_config->mspi_nor_cfg.tx_dummy;
	dev_data->xfer.rx_dummy = (cmd->dir == MSPI_RX) ?
				  cmd->rx_dummy : dev_config->mspi_nor_cfg.rx_dummy;

	dev_data->packet.dir = cmd->dir;
	dev_data->packet.cmd = cmd->cmd;
}

void flash_mspi_command_set_dma(const struct device *dev, const struct flash_mspi_nor_cmd *cmd)
{
	struct flash_mspi_nor_data *dev_data = dev->data;
	const struct flash_mspi_nor_config *dev_config = dev->config;

	memset(&dev_data->xfer, 0, sizeof(dev_data->xfer));
	memset(&dev_data->packet, 0, sizeof(dev_data->packet));

	dev_data->xfer.xfer_mode  = MSPI_DMA;
	dev_data->xfer.packets    = &dev_data->packet;
	dev_data->xfer.num_packet = 1;
	dev_data->xfer.timeout    = 10;

	dev_data->xfer.cmd_length = cmd->cmd_length;
	dev_data->xfer.addr_length = cmd->addr_length;
	dev_data->xfer.tx_dummy = (cmd->dir == MSPI_TX) ?
				  cmd->tx_dummy : dev_config->mspi_nor_cfg.tx_dummy;
	dev_data->xfer.rx_dummy = (cmd->dir == MSPI_RX) ?
				  cmd->rx_dummy : dev_config->mspi_nor_cfg.rx_dummy;

	dev_data->packet.dir = cmd->dir;
	dev_data->packet.cmd = cmd->cmd;
}

static int dev_cfg_apply(const struct device *dev, const struct mspi_dev_cfg *cfg)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	int rc = mspi_dev_config(dev_config->bus, &dev_config->mspi_id,
				 MSPI_DEVICE_CONFIG_ALL, cfg);
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	if (rc < 0) {
		LOG_ERR("Failed to set device config: %p error: %d", cfg, rc);
	}
	return rc;
}

static int acquire(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	int rc = 0;

	k_sem_take(&dev_data->acquired, K_FOREVER);

	if (rc < 0) {
		printf("pm_device_runtime_get() failed: %d", rc);
	} else {
		/* This acquires the MSPI controller and reconfigures it
		 * if needed for the flash device.
		 */


		if (rc < 0) {
			printf("mspi_dev_config() failed: %d", rc);

		} else {

			return 0;
		}

	}

	k_sem_give(&dev_data->acquired);
	return rc;
}

static void release(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;

	/* This releases the MSPI controller. */
	// (void)mspi_get_channel_status(dev_config->bus, 0);

	// (void)pm_device_runtime_put(dev_config->bus);

	k_sem_give(&dev_data->acquired);
}

static inline uint32_t dev_flash_size(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;

	return dev_config->flash_size;
}

static inline uint16_t dev_page_size(const struct device *dev)
{
	return SPI_NOR_PAGE_SIZE;
}

static int api_read(const struct device *dev, off_t addr, void *dest,
		    size_t size)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	const uint32_t flash_size = dev_flash_size(dev);
	uint8_t *dma_buf = (uint8_t * )(0xfffd0000);
	int rc;
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	if (size == 0) {
		return 0;
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	if ((addr < 0) || ((addr + size) > flash_size)) {
		return -EINVAL;
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	rc = acquire(dev);
	if (rc < 0) {
		return rc;
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	// if (dev_config->jedec_cmds->read.force_single) {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
	// } else {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_cfg);
	// }

	if (rc < 0) {
		return rc;
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);
	if (IS_ENABLED (DMA_MODE_RD)) {
		memset (dma_buf , 0x00, 32);
		for (int i = 0; i < 32; i++) {
			printf ("***[%s], [%s], [%04d], dma_buf is %x\r\n", __FILE__, __func__, __LINE__, dma_buf[i]);
		}
	}

#ifdef IO_MODE
	if (IS_ENABLED(IO_MODE)) {
		flash_mspi_command_set(dev, &dev_config->jedec_cmds->read);
	} else if (IS_ENABLED(DMA_MODE_RD)) {
		flash_mspi_command_set_dma(dev, &dev_config->jedec_cmds->read);
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	dev_data->packet.address   = addr;
	dev_data->packet.data_buf  = DMA_MODE_RD == 1 ? dma_buf : dest;
	dev_data->packet.num_bytes = size;
	rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
			     &dev_data->xfer);

	if (IS_ENABLED (DMA_MODE_RD)) {
		for (int i = 0; i < 32; i++) {
			printf ("***[%s], [%s], [%04d], dma_buf is %x\r\n", __FILE__, __func__, __LINE__, dma_buf[i]);
		}
	}
#elif XIP_MODE
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	// NO hurry to make it work
	// memcpy(dest, (0x60000000 + addr), size);
#endif

	release(dev);
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	if (rc < 0) {
		printf("Read xfer failed: %d", rc);
		return rc;
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	return 0;
}

static int status_get(const struct device *dev, uint8_t *status)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	int rc;

	/* Enter command mode */
	// if (dev_config->jedec_cmds->status.force_single) {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
	// } else {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_cfg);
	// }

	if (rc < 0) {
		LOG_ERR("Switching to dev_cfg failed: %d", rc);
		return rc;
	}

	flash_mspi_command_set(dev, &dev_config->jedec_cmds->status);
	dev_data->packet.data_buf  = status;
	dev_data->packet.num_bytes = sizeof(uint8_t);

	rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id, &dev_data->xfer);

	if (rc < 0) {
		LOG_ERR("Status xfer failed: %d", rc);
		return rc;
	}

	return rc;
}

static int wait_until_ready(const struct device *dev, k_timeout_t poll_period)
{
	int rc;
	uint8_t status_reg;

	while (true) {
		rc = status_get(dev, &status_reg);

		if (rc < 0) {
			LOG_ERR("Wait until ready - status xfer failed: %d", rc);
			return rc;
		}

		if (!(status_reg & SPI_NOR_WIP_BIT)) {
			break;
		}

		k_sleep(poll_period);
	}

	return 0;
}

static int write_enable(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	int rc;

	// if (dev_config->jedec_cmds->write_en.force_single) {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
	// } else {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_cfg);
	// }

	if (rc < 0) {
		return rc;
	}

	flash_mspi_command_set(dev, &dev_config->jedec_cmds->write_en);
	return mspi_transceive(dev_config->bus, &dev_config->mspi_id, &dev_data->xfer);
}

static int api_write(const struct device *dev, off_t addr, const void *src,
		     size_t size)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	const uint32_t flash_size = dev_flash_size(dev);
	const uint16_t page_size = dev_page_size(dev);
	int rc;

	if (size == 0) {
		return 0;
	}

	if ((addr < 0) || ((addr + size) > flash_size)) {
		return -EINVAL;
	}

	rc = acquire(dev);
	if (rc < 0) {
		return rc;
	}

	while (size > 0) {
		/* Split write into parts, each within one page only. */
		uint16_t page_offset = (uint16_t)(addr % page_size);
		uint16_t page_left = page_size - page_offset;
		uint16_t to_write = (uint16_t)MIN(size, page_left);

		if (write_enable(dev) < 0) {
			LOG_ERR("Write enable xfer failed: %d", rc);
			break;
		}

		// if (dev_config->jedec_cmds->page_program.force_single) {
		// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
		// } else {
		// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_cfg);
		// }

		if (rc < 0) {
			return rc;
		}

#ifdef IO_MODE
		if (IS_ENABLED(IO_MODE)) {
			flash_mspi_command_set(dev, &dev_config->jedec_cmds->page_program);
		} else if (IS_ENABLED(DMA_MODE_WR)) {
			flash_mspi_command_set_dma(dev, &dev_config->jedec_cmds->page_program);
		}

		dev_data->packet.address   = addr;
		dev_data->packet.data_buf  = (uint8_t *)src;
		dev_data->packet.num_bytes = to_write;
		rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
				     &dev_data->xfer);
		if (rc < 0) {
			LOG_ERR("Page program xfer failed: %d", rc);
			break;
		}
#elif XIP_MODE
		// memcpy ((uint8_t *)(0x60000000 + addr), src, to_write);
#endif

		addr += to_write;
		src   = (const uint8_t *)src + to_write;
		size -= to_write;

		rc = wait_until_ready(dev, K_MSEC(1));
		if (rc < 0) {
			break;
		}
	}

	release(dev);

	return rc;
}

static int api_erase(const struct device *dev, off_t addr, size_t size)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	const uint32_t flash_size = dev_flash_size(dev);
	int rc = 0;

	if ((addr < 0) || ((addr + size) > flash_size)) {
		return -EINVAL;
	}

	rc = acquire(dev);
	if (rc < 0) {
		return rc;
	}

	while (size > 0) {
		rc = write_enable(dev);
		if (rc < 0) {
			printf("Write enable failed.");
			break;
		}

		// /* Sector erase. */
		// if (dev_config->jedec_cmds->sector_erase.force_single) {
		// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
		// } else {
		// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_cfg);
		// }

		if (rc < 0) {
			return rc;
		}

		flash_mspi_command_set(dev, &dev_config->jedec_cmds->sector_erase);
		dev_data->packet.address = addr;
		addr += SPI_NOR_SECTOR_SIZE;
		size -= SPI_NOR_SECTOR_SIZE;

		rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
				     &dev_data->xfer);
		if (rc < 0) {
			printf("Erase command 0x%02x xfer failed: %d",
				dev_data->packet.cmd, rc);
			break;
		}

		rc = wait_until_ready(dev, K_MSEC(1));
		if (rc < 0) {
			break;
		}
	}

	release(dev);
	return rc;
}

static const
struct flash_parameters *api_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	static const struct flash_parameters parameters = {
		.write_block_size = 1,
		.erase_value = 0xff,
	};

	return &parameters;
}

static int read_jedec_id(const struct device *dev, uint8_t *id)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	int rc;

	// if (dev_config->jedec_cmds->id.force_single) {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
	// } else {
	// 	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_cfg);
	// }

	if (rc < 0) {
		return rc;
	}

	flash_mspi_command_set(dev, &dev_config->jedec_cmds->id);
	dev_data->packet.data_buf  = id;
	dev_data->packet.num_bytes = JESD216_READ_ID_LEN;

	rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
			     &dev_data->xfer);
	if (rc < 0) {
		LOG_ERR("Read JEDEC ID failed: %d\n", rc);
	}

	return rc;
}

static int dev_pm_action_cb(const struct device *dev,
			    enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		break;
	case PM_DEVICE_ACTION_RESUME:
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int octal_enable_set(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	int rc;

	flash_mspi_command_set(dev, &commands_single.write_en);

	rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
			     &dev_data->xfer);
	if (rc < 0) {
		LOG_ERR("Failed to set write enable: %d", rc);
		return rc;
	}

	uint8_t value = DOPI_MODE ? 0x02 : 0x01;
	uint32_t addr = 0;
	flash_mspi_command_set(dev, &commands_single.wrcr2);

	dev_data->packet.data_buf  = &value;
	dev_data->packet.address  = addr;
	dev_data->packet.num_bytes = 1;

	rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
			     &dev_data->xfer);
}

static int quad_enable_set(const struct device *dev, bool enable)
{
	// const struct flash_mspi_nor_config *dev_config = dev->config;
	// struct flash_mspi_nor_data *dev_data = dev->data;
	// int rc;

	// flash_mspi_command_set(dev, &commands_single.write_en);

	// // There is no need to define data_buf and num_bytes, only command code valid.
	// rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
	// 		     &dev_data->xfer);
	// if (rc < 0) {
	// 	LOG_ERR("Failed to set write enable: %d", rc);
	// 	return rc;
	// }

	// uint8_t value = 0x02;
	// uint32_t addr = 0;
	// flash_mspi_command_set(dev, &commands_single.wrcr2);

	// dev_data->packet.data_buf  = &value;
	// dev_data->packet.address  = addr;
	// dev_data->packet.num_bytes = 1;

	// rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
	// 		     &dev_data->xfer);
}

static int default_io_mode(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	enum mspi_io_mode io_mode = dev_config->mspi_nor_cfg.io_mode;
	int rc = 0;
	uint8_t *buf = (uint8_t *)k_malloc(0x1000);	
	uint8_t *buf_wr = (uint8_t *)k_malloc(0x1000);
	memset (buf , 0x00, 32);
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	for (int n = 0; n < 32; n++) {
		buf_wr[n] = rand() % 0x40;
	}

	if (IS_ENABLED (IO_MODE)) {
		printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);
		rc = dev_cfg_apply(dev, &mspi_dev_cfg_xip);
		printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

		flash_mspi_command_set(dev, &commands_single.write_en);

		rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
					&dev_data->xfer);

		flash_mspi_command_set(dev, &commands_single.page_program);

		dev_data->packet.data_buf  = buf_wr;
		dev_data->packet.address  = 0;
		dev_data->packet.num_bytes = 32;
		rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
					&dev_data->xfer);

		rc = wait_until_ready(dev, K_MSEC(1));

		flash_mspi_command_set(dev, &commands_single.read);

		dev_data->packet.data_buf  = buf;
		dev_data->packet.address  = 0;
		dev_data->packet.num_bytes = 32;
		rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
					&dev_data->xfer);

		for (int i = 0; i < 32; i++) {
			printf ("***[%s], [%s], [%04d], buf is %x\r\n", __FILE__, __func__, __LINE__, buf[i]);
		}

		if (IS_ENABLED (OCTA_MODE)) {
			memset (buf , 0x00, 32);
			rc = octal_enable_set(dev);
			rc = dev_cfg_apply(dev, (OCTA_MODE ? &mspi_dev_cfg_octal : &mspi_dev_cfg_xip));
			// flash_mspi_command_set(dev, &commands_octal.read);

			// dev_data->packet.data_buf  = buf;
			// dev_data->packet.address  = 0;
			// dev_data->packet.num_bytes = 32;
			// rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
			// 			&dev_data->xfer);

			// for (int i = 0; i < 32; i++) {
			// 	printf ("***[%s], [%s], [%04d], buf is %x\r\n", __FILE__, __func__, __LINE__, buf[i]);
			// }	
		}
	} else if (IS_ENABLED (XIP_MODE)) {
		if (IS_ENABLED (OCTA_MODE)) {
			rc = dev_cfg_apply(dev, &mspi_dev_cfg_xip);
			rc = octal_enable_set(dev);
		}

		rc = dev_cfg_apply(dev, (OCTA_MODE ? &mspi_dev_cfg_octal : &mspi_dev_cfg_xip));
		rc = mspi_xip_config(dev_config->bus, &dev_config->mspi_id,
			&mspi_xip_cfg);
		memcpy(buf, 0x60000000, 32);
		for (int i = 0; i < 32; i++) {
			printf ("***[%s], [%s], [%04d], buf is %x\r\n", __FILE__, __func__, __LINE__, buf[i]);
		}

		for (int i = 0; i < 32; i++) {
			printf ("***[%s], [%s], [%04d], buf_wr is %x\r\n", __FILE__, __func__, __LINE__, buf_wr[i]);
		}

		// need WREN first
		// memcpy(0x60000000, buf_wr, 32);
	}

	if (IS_ENABLED (OCTA_MODE)) {
		// rc = octal_enable_set(dev);
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);
	// uint32_t read_id = 0;
	// flash_mspi_command_set(dev, &commands_octal.id);

	// dev_data->packet.data_buf  = &read_id;
	// dev_data->packet.address  = 0;
	// dev_data->packet.num_bytes = 2;
	// rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
	// 		     &dev_data->xfer);

	return 0;
}

static int flash_chip_init(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	enum mspi_io_mode io_mode = dev_config->mspi_nor_cfg.io_mode;
	uint8_t id[JESD216_READ_ID_LEN] = {0};
	int rc;

	// DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	// device_map(&dev_data->flash_mmio, UEFC_BASE_MAP_ADDR, UEFC_MAP_SIZE, K_MEM_CACHE_NONE);
	// device_map(&dev_data->sram_mmio, 0xFFFC0000, 0x40000, K_MEM_CACHE_NONE);

	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);

	if (rc < 0) {
		return rc;
	}

#if !TEST_MODE
#if READ_ID_FORCE_SINGLE
	flash_mspi_command_set(dev, &commands_single.id);
	dev_data->packet.data_buf  = id;
	dev_data->packet.num_bytes = sizeof(id);

	rc = mspi_transceive(dev_config->bus, &dev_config->mspi_id,
			     &dev_data->xfer);
	
	printf ("***[%s], [%s], [%04d], id is %x\r\n", __FILE__, __func__, __LINE__, id[0]);
#endif
#endif

	rc = dev_cfg_apply(dev, &dev_config->mspi_nor_init_cfg);
	rc = default_io_mode(dev);

	/* Reading JEDEC ID for mode that forces single lane would be redundant,
	 * since it switches back to single lane mode. Use ID from previous read.
	 */
	// if (!dev_config->jedec_cmds->id.force_single) {

	// 	rc = read_jedec_id(dev, id);
	// 	if (rc < 0) {
	// 		LOG_ERR("Failed to read JEDEC ID in final line mode: %d", rc);
	// 		return rc;
	// 	}
	// }

	// if (memcmp(id, dev_config->jedec_id, sizeof(id)) != 0) {
	// 	LOG_ERR("JEDEC ID mismatch, read: %02x %02x %02x, "
	// 		"expected: %02x %02x %02x",
	// 		id[0], id[1], id[2],
	// 		dev_config->jedec_id[0],
	// 		dev_config->jedec_id[1],
	// 		dev_config->jedec_id[2]);
	// 	return -ENODEV;
	// }

#if defined(CONFIG_MSPI_XIP)
	// /* Enable XIP access for this chip if specified so in DT. */
	// if (dev_config->xip_cfg.enable) {
	// 	rc = mspi_xip_config(dev_config->bus, &dev_config->mspi_id,
	// 			     &dev_config->xip_cfg);
	// 	if (rc < 0) {
	// 		return rc;
	// 	}
	// }
#endif

	return 0;
}

static int drv_init(const struct device *dev)
{
	const struct flash_mspi_nor_config *dev_config = dev->config;
	struct flash_mspi_nor_data *dev_data = dev->data;
	int rc;

	if (!device_is_ready(dev_config->bus)) {
		LOG_ERR("Device %s is not ready", dev_config->bus->name);
		return -ENODEV;
	}

	rc = flash_chip_init(dev);

	if (rc < 0) {
		return rc;
	}
	k_sem_init(&dev_data->acquired, 1, K_SEM_MAX_LIMIT);

	return 0;
}

static DEVICE_API(flash, drv_api) = {
	.read = api_read,
	.write = api_write,
	.erase = api_erase,
	.get_parameters = api_get_parameters,
};

#define FLASH_INITIAL_CONFIG(inst)					\
{									\
	.ce_num = DT_INST_PROP_OR(inst, mspi_hardware_ce_num, 0),	\
	.freq = MIN(DT_INST_PROP(inst, mspi_max_frequency), MHZ(50)),	\
	.io_mode = MSPI_IO_MODE_SINGLE,					\
	.data_rate = MSPI_DATA_RATE_SINGLE,				\
	.cpp = MSPI_CPP_MODE_0,						\
	.endian = MSPI_XFER_BIG_ENDIAN,					\
	.ce_polarity = MSPI_CE_ACTIVE_LOW,				\
	.dqs_enable = false,						\
}

#define FLASH_SIZE_INST(inst) (DT_INST_PROP(inst, size) / 8)

/* Define copies of mspi_io_mode enum values, so they can be used inside
 * the COND_CODE_1 macros.
 */
#define _MSPI_IO_MODE_SINGLE 0
#define _MSPI_IO_MODE_QUAD_1_4_4 6
#define _MSPI_IO_MODE_OCTAL 7
#define _MSPI_IO_MODE_DUAL_1_2_2 3
BUILD_ASSERT(_MSPI_IO_MODE_SINGLE == MSPI_IO_MODE_SINGLE,
	"Please align _MSPI_IO_MODE_SINGLE macro value");
BUILD_ASSERT(_MSPI_IO_MODE_QUAD_1_4_4 == MSPI_IO_MODE_QUAD_1_4_4,
	"Please align _MSPI_IO_MODE_QUAD_1_4_4 macro value");
BUILD_ASSERT(_MSPI_IO_MODE_OCTAL == MSPI_IO_MODE_OCTAL,
	"Please align _MSPI_IO_MODE_OCTAL macro value");
// BUILD_ASSERT(_MSPI_IO_MODE_DUAL_1_2_2 == MSPI_IO_MODE_DUAL_1_2_2,
// 	"Please align _MSPI_IO_MODE_OCTAL macro value");

/* Define a non-existing extern symbol to get an understandable compile-time error
 * if the IO mode is not supported by the driver.
 */
extern const struct flash_mspi_nor_cmds mspi_io_mode_not_supported;

#define FLASH_CMDS(inst) COND_CODE_1( \
	IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_SINGLE), \
	(&commands_single), \
	(COND_CODE_1( \
		IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_QUAD_1_4_4), \
		(&commands_quad_1_4_4), \
		(COND_CODE_1( \
			IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_OCTAL), \
			(&commands_octal), \
			(&mspi_io_mode_not_supported) \
		)) \
	)) \
)

// #define FLASH_CMDS(inst) COND_CODE_1( \
// 	IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_SINGLE), \
// 	(&commands_single), \
// 	(COND_CODE_1( \
// 		IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_QUAD_1_4_4), \
// 		(&commands_quad_1_4_4), \
// 		(COND_CODE_1( \
// 			IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_OCTAL), \
// 			(&commands_octal), \
// 			(COND_CODE_1(\
// 				IS_EQ(DT_INST_ENUM_IDX(inst, mspi_io_mode), _MSPI_IO_MODE_1_2_2), \
// 				(&commands_1_2_2), \
// 				(&mspi_io_mode_not_supported) \
// 			))\
// 		)) \
// 	)) \
// )

#define FLASH_QUIRKS(inst) FLASH_MSPI_QUIRKS_GET(DT_DRV_INST(inst))

#define FLASH_DW15_QER_VAL(inst) _CONCAT(JESD216_DW15_QER_VAL_, \
	DT_INST_STRING_TOKEN(inst, quad_enable_requirements))
#define FLASH_DW15_QER(inst) COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, quad_enable_requirements), \
	(FLASH_DW15_QER_VAL(inst)), (JESD216_DW15_QER_VAL_NONE))


#if defined(CONFIG_FLASH_PAGE_LAYOUT)
BUILD_ASSERT((CONFIG_FLASH_MSPI_NOR_LAYOUT_PAGE_SIZE % 4096) == 0,
	"MSPI_NOR_FLASH_LAYOUT_PAGE_SIZE must be multiple of 4096");
#define FLASH_PAGE_LAYOUT_DEFINE(inst) \
	.layout = { \
		.pages_size = CONFIG_FLASH_MSPI_NOR_LAYOUT_PAGE_SIZE, \
		.pages_count = FLASH_SIZE_INST(inst) \
			     / CONFIG_FLASH_MSPI_NOR_LAYOUT_PAGE_SIZE, \
	},
#define FLASH_PAGE_LAYOUT_CHECK(inst) \
BUILD_ASSERT((FLASH_SIZE_INST(inst) % CONFIG_FLASH_MSPI_NOR_LAYOUT_PAGE_SIZE) == 0, \
	"MSPI_NOR_FLASH_LAYOUT_PAGE_SIZE incompatible with flash size, instance " #inst);
#else
#define FLASH_PAGE_LAYOUT_DEFINE(inst)
#define FLASH_PAGE_LAYOUT_CHECK(inst)
#endif

/* MSPI bus must be initialized before this device. */
#if (CONFIG_MSPI_INIT_PRIORITY < CONFIG_FLASH_INIT_PRIORITY)
#define INIT_PRIORITY CONFIG_FLASH_INIT_PRIORITY
#else
#define INIT_PRIORITY UTIL_INC(CONFIG_MSPI_INIT_PRIORITY)
#endif

#define FLASH_MSPI_NOR_INST(inst)						\
	BUILD_ASSERT((DT_INST_ENUM_IDX(inst, mspi_io_mode) ==			\
		      MSPI_IO_MODE_SINGLE) ||					\
		     (DT_INST_ENUM_IDX(inst, mspi_io_mode) ==			\
		      MSPI_IO_MODE_QUAD_1_4_4) ||				\
		     (DT_INST_ENUM_IDX(inst, mspi_io_mode) ==			\
		      MSPI_IO_MODE_OCTAL) ||					\
			 (DT_INST_ENUM_IDX(inst, mspi_io_mode) ==			\
		      MSPI_IO_MODE_DUAL_1_2_2),					\
		"Only 1x, 1-4-4 and 8x I/O modes are supported for now");	\
	PM_DEVICE_DT_INST_DEFINE(inst, dev_pm_action_cb);			\
	static struct flash_mspi_nor_data dev##inst##_data;			\
	static const struct flash_mspi_nor_config dev##inst##_config = {	\
		._mmio = Z_DEVICE_MMIO_ROM_INITIALIZER(DT_DRV_INST(inst)),\
		.bus = DEVICE_DT_GET(DT_INST_BUS(inst)),			\
		.flash_size = FLASH_SIZE_INST(inst),				\
		.mspi_id = MSPI_DEVICE_ID_DT_INST(inst),			\
		.mspi_nor_cfg = MSPI_DEVICE_CONFIG_DT_INST(inst),		\
		.mspi_nor_init_cfg = FLASH_INITIAL_CONFIG(inst),		\
		.mspi_nor_cfg_mask = DT_PROP(DT_INST_BUS(inst),			\
					 software_multiperipheral)		\
			       ? MSPI_DEVICE_CONFIG_ALL				\
			       : MSPI_DEVICE_CONFIG_NONE,			\
	IF_ENABLED(CONFIG_MSPI_XIP,						\
		(.xip_cfg = MSPI_XIP_CONFIG_DT_INST(inst),))			\
	IF_ENABLED(WITH_RESET_GPIO,						\
		(.reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),	\
		.reset_pulse_us = DT_INST_PROP_OR(inst, t_reset_pulse, 0)	\
				/ 1000,						\
		.reset_recovery_us = DT_INST_PROP_OR(inst, t_reset_recovery, 0)	\
				   / 1000,))					\
		FLASH_PAGE_LAYOUT_DEFINE(inst)					\
		.jedec_id = DT_INST_PROP(inst, jedec_id),			\
		.jedec_cmds = FLASH_CMDS(inst),					\
		.quirks = FLASH_QUIRKS(inst),					\
		.dw15_qer = FLASH_DW15_QER(inst),				\
	};									\
	FLASH_PAGE_LAYOUT_CHECK(inst)						\
	DEVICE_DT_INST_DEFINE(inst,						\
		drv_init, PM_DEVICE_DT_INST_GET(inst),				\
		&dev##inst##_data, &dev##inst##_config,				\
		POST_KERNEL, INIT_PRIORITY,					\
		&drv_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_MSPI_NOR_INST)
