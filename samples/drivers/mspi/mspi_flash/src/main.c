/*
 * Copyright (c) 2024 Ambiq Micro Inc. <www.ambiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <stdio.h>
#include <string.h>

#if DT_HAS_COMPAT_STATUS_OKAY(mxicy_mspi_controller)
#define XLNX_MSPI_COMPAT mxicy_mspi_controller
#else
#define XLNX_MSPI_COMPAT invalid
#endif

#define SPI_FLASH_TEST_REGION_OFFSET 0

#define SPI_FLASH_SECTOR_SIZE        32

#define FLASH_ERASE_SECTOR_SIZE        4096

#define SPI_FLASH_MULTI_SECTOR_TEST

#define SIGLE_SECTOR_TEST_ENABLE 0
#define DMA_MODE 1

int single_sector_test(const struct device *flash_dev)
{
	// const uint8_t expected[] = { 0x55, 0xaa, 0x66, 0x99 };
	// const size_t len = sizeof(expected);
	// uint8_t buf[sizeof(expected)];
	// int rc;

	const size_t len = SPI_FLASH_SECTOR_SIZE;
	uint8_t buf[SPI_FLASH_SECTOR_SIZE];
	int rc;
	uint8_t *erased = (uint8_t * )malloc (SPI_FLASH_SECTOR_SIZE);	
	volatile uint8_t *buf_rd = (uint8_t * )malloc (SPI_FLASH_SECTOR_SIZE);
	volatile uint8_t *buf_wr = (uint8_t * )malloc (SPI_FLASH_SECTOR_SIZE);

	memset (erased, 0xff, SPI_FLASH_SECTOR_SIZE);
	memset (buf, 0x00, 32);
	for (int n = 0; n < SPI_FLASH_SECTOR_SIZE; n++) {
		buf_wr[n] = rand() % 0x30;
	}

	printf("\nPerform test on single sector");
	/* Write protection needs to be disabled before each write or
	 * erase, since the flash component turns on write protection
	 * automatically after completion of write and erase
	 * operations.
	 */
	printf("\nTest 1: Flash erase\n");
	uint8_t * dma_buf = (uint8_t *)0x200000;

	/* Full flash erase if SPI_FLASH_TEST_REGION_OFFSET = 0 and
	 * SPI_FLASH_SECTOR_SIZE = flash size
	 */
	memset(buf_rd, 0, len);

	for (int i = 0; i < 32; i++) {
		printf ("***[%s], [%s], [%04d], buf_rd is %x\r\n", __FILE__, __func__, __LINE__, buf[i]);
	}

	rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, buf, len);
	if (rc != 0) {
		printf("Flash read failed! %d\n", rc);
		return 1;
	}

	for (int i = 0; i < 32; i++) {
		printf ("***[%s], [%s], [%04d], buf_rd is %x\r\n", __FILE__, __func__, __LINE__, buf[i]);
	}

	rc = flash_erase(flash_dev, SPI_FLASH_TEST_REGION_OFFSET,
			 FLASH_ERASE_SECTOR_SIZE);

	if (rc != 0) {
		printf("Flash erase failed! %d\n", rc);
	} else {
		printf("Flash erase succeeded!\n");
	}
	
	rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, buf, len);
	if (rc != 0) {
		printf("Flash read failed! %d\n", rc);
		return 1;
	}

	// for (int i = 0; i < 32; i++) {
	// 	printf ("***[%s], [%s], [%04d], buf_rd is %x\r\n", __FILE__, __func__, __LINE__, buf[i]);
	// }

	printf("\nTest 2: Flash write\n");

	printf("Attempting to write %zu bytes\n", len);
	rc = flash_write(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, buf_wr, len);
	if (rc != 0) {
		printf("Flash write failed! %d\n", rc);
		return 1;
	}

	memset(buf, 0, len);
	memset(buf_rd, 0, len);
	memset(dma_buf, 0, len);

	rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, \
				(DMA_MODE == 1) ? dma_buf : buf, len);
	if (rc != 0) {
		printf("Flash read failed! %d\n", rc);
		return 1;
	}

	if (memcmp(buf_wr, (DMA_MODE == 1) ? dma_buf : buf, len) == 0) {
		printf("Data read matches data written. Good!!\n");
	} else {
		const uint8_t *wp = buf_wr;
		const uint8_t *rp = (DMA_MODE == 1) ? dma_buf : buf;
		const uint8_t *rpe = rp + len;

		printf("Data read does not match data written!!\n");
		while (rp < rpe) {
			printf("%08x wrote %02x read %02x %s\n",
			       (uint32_t)(SPI_FLASH_TEST_REGION_OFFSET + (rp - (DMA_MODE == 1) ? dma_buf : buf)),
			       *wp, *rp, (*rp == *wp) ? "match" : "MISMATCH");
			++rp;
			++wp;
		}
	}

	return rc;
}

int main(void)
{
	unsigned int sctlr = __get_SCTLR();
	printf ("***[%s], [%s], [%04d], sctlr is %x\r\n", __FILE__, __func__, __LINE__, sctlr);

	const struct device *mspi_dev = DEVICE_DT_GET_ONE(XLNX_MSPI_COMPAT);

	if (!mspi_dev) {
 		printf ("*** NO MSPI dev found! \r\n");
		return;
	}

	printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);

	device_init(mspi_dev);

	const struct device *flash_dev = DEVICE_DT_GET_ONE(jedec_mspi_nor);
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);
	if (!flash_dev) {
 		printf ("***NO Flash dev found! \r\n");
		return;
	}
printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);
	device_init(flash_dev);

	if (!device_is_ready(flash_dev)) {
		printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);
		printk("%s: device not ready.\n", flash_dev->name);
	return 0;
}

printf ("***[%s], [%s], [%04d],\r\n", __FILE__, __func__, __LINE__);
#if SIGLE_SECTOR_TEST_ENABLE
	if (single_sector_test(flash_dev)) {
		return 1;
	}
#endif

// #if defined SPI_FLASH_MULTI_SECTOR_TEST
// 	if (multi_sector_test(flash_dev)) {
// 		return 1;
// 	}
// #endif

	return 0;
}
