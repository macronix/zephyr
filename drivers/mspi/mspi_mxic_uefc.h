/*
 * Copyright (c) 2024, Ambiq Micro Inc. <www.ambiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_instance.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys_clock.h>
#include <stdlib.h>
#include <zephyr/irq.h>
#include <zephyr/sys/byteorder.h>

enum HC_XFER_MODE_TYPE {
	HC_XFER_MODE_IO,
	HC_XFER_MODE_MAP,
	HC_XFER_MODE_DMA,
	MAX_HC_XFER_MODE
};

#define CHIP_SELECT_COUNT               3u
#define SPI_WORD_SIZE                   8u
#define SPI_WR_RD_CHUNK_SIZE_MAX        16u
#define MXIC_UEFC_CMD_LENGTH   2
#define MXIC_UEFC_ADDR_LENGTH   4

#define BIT(x) (1U << (x))
/* Host Controller Register */
#define HC_CTRL						0x00
#define HC_CTRL_RQE_EN				BIT(31)
#define HC_CTRL_SDMA_BD(x)			(((x) & 0x7) << 28)
#define HC_CTRL_PARALLEL_1			BIT(27)
#define HC_CTRL_PARALLEL_0			BIT(26)
#define HC_CTRL_DATA_ORDER			BIT(25) //OctaFlash, OctaRAM
#define HC_CTRL_SIO_SHIFTER(x)		(((x) & 0x3) << 23)
#define HC_CTRL_EX_SER_B			BIT(22)
#define HC_CTRL_EX_SER_A			BIT(21)
#define HC_CTRL_ASSIMI_BYTE_B(x)	(((x) & 0x3) << 19)
#define HC_CTRL_ASSIMI_BYTE_A(x)	(((x) & 0x3) << 17)
#define HC_CTRL_EX_PHY_ITE_B		BIT(16)
#define HC_CTRL_EX_PHY_ITE_A		BIT(15)
#define HC_CTRL_EX_PHY_DQS_B		BIT(14)
#define HC_CTRL_EX_PHY_DQS_A		BIT(13)
#define HC_CTRL_LED					BIT(12)
#define HC_CTRL_CH_SEL_B			BIT(11)
#define HC_CTRL_CH_SEL_A			0
#define HC_CTRL_CH_MASK				BIT(11)
#define HC_CTRL_LUN_SEL(x)			(((x) & 0x7) << 8) //NAND
#define HC_CTRL_LUN_MASK			HC_CTRL_LUN_SEL(0x7)
#define HC_CTRL_PORT_SEL(x)			(((x) & 0xff) << 0)
#define HC_CTRL_PORT_MASK			(HC_CTRL_PORT_SEL(0xff))

#define HC_CMD_LENGTH_MASK          OP_CMD_CNT(0x7)
#define HC_ADDR_LENGTH_MASK         OP_ADDR_CNT(0x7)

#define HC_CTRL_CH_LUN_PORT_MASK	(HC_CTRL_CH_MASK | HC_CTRL_LUN_MASK | HC_CTRL_PORT_MASK)
#define HC_CTRL_CH_LUN_PORT(ch, lun, port) (HC_CTRL_CH_SEL_##ch | HC_CTRL_LUN_SEL(lun) | HC_CTRL_PORT_SEL(port))

/* Normal Interrupt Status Register */
#define INT_STS				0x04
#define INT_STS_CA_REQ			BIT(30)
#define INT_STS_CACHE_RDY		BIT(29)
#define INT_STS_AC_RDY			BIT(28)
#define INT_STS_ERR_INT			BIT(15)
#define INT_STS_CQE_INT			BIT(14)
#define INT_STS_DMA_TFR_CMPLT		BIT(7)
#define INT_STS_DMA_INT			BIT(6)
#define INT_STS_BUF_RD_RDY		BIT(5)
#define INT_STS_BUF_WR_RDY		BIT(4)
#define INT_STS_ALL_CLR 		(INT_STS_AC_RDY | \
					INT_STS_ERR_INT | \
					INT_STS_DMA_TFR_CMPLT | \
					INT_STS_DMA_INT)

/* Error Interrupt Status Register */
#define ERR_INT_STS			0x08
#define ERR_INT_STS_ECC			BIT(19)
#define ERR_INT_STS_PREAM		BIT(18)
#define ERR_INT_STS_CRC			BIT(17)
#define ERR_INT_STS_AC			BIT(16)
#define ERR_INT_STS_ADMA		BIT(9)
#define ERR_INT_STS_AUTO_CMD		BIT(8)
#define ERR_INT_STS_DATA_END		BIT(6)
#define ERR_INT_STS_DATA_CRC		BIT(5)
#define ERR_INT_STS_DATA_TIMEOUT	BIT(4)
#define ERR_INT_STS_CMD_IDX		BIT(3)
#define ERR_INT_STS_CMD_END		BIT(2)
#define ERR_INT_STS_CMD_CRC		BIT(1)
#define ERR_INT_STS_CMD_TIMEOUT		BIT(0)
#define ERR_INT_STS_ALL_CLR		(ERR_INT_STS_ECC | \
					ERR_INT_STS_PREAM | \
					ERR_INT_STS_CRC | \
					ERR_INT_STS_AC | \
					ERR_INT_STS_ADMA)

/* Normal Interrupt Status Enable Register */
#define INT_STS_EN			0x0C
#define INT_STS_EN_CA_REQ		BIT(30)
#define INT_STS_EN_CACHE_RDY		BIT(29)
#define INT_STS_EN_AC_RDY		BIT(28)
#define INT_STS_EN_ERR_INT		BIT(15)
#define INT_STS_EN_DMA_TFR_CMPLT	BIT(7)
#define INT_STS_DMA					BIT(6)
#define INT_STS_EN_BUF_RD_RDY		BIT(5)
#define INT_STS_EN_BUF_WR_RDY		BIT(4)
#define INT_STS_EN_DMA_INT		BIT(3)
#define INT_STS_EN_BLK_GAP		BIT(2)
#define INT_STS_EN_DAT_CMPLT		BIT(1)
#define INT_STS_EN_CMD_CMPLT		BIT(0)
#define INT_STS_EN_ALL_EN		(INT_STS_EN_AC_RDY | \
					INT_STS_EN_ERR_INT | \
					INT_STS_EN_DMA_TFR_CMPLT | \
					INT_STS_EN_DMA_INT)

/* Error Interrupt Status Enable Register */
#define ERR_INT_STS_EN			0x10
#define ERR_INT_STS_EN_ECC		BIT(19)
#define ERR_INT_STS_EN_PREAM		BIT(18)
#define ERR_INT_STS_EN_CRC		BIT(17)
#define ERR_INT_STS_EN_AC		BIT(16)
#define ERR_INT_STS_EN_ADMA		BIT(9)
#define ERR_INT_STS_EN_AUTO_CMD		BIT(8)
#define ERR_INT_STS_EN_DATA_END		BIT(6)
#define ERR_INT_STS_EN_DATA_CRC		BIT(5)
#define ERR_INT_STS_EN_DATA_TIMEOUT	BIT(4)
#define ERR_INT_STS_EN_CMD_IDX		BIT(3)
#define ERR_INT_STS_EN_CMD_END		BIT(2)
#define ERR_INT_STS_EN_CMD_CRC		BIT(1)
#define ERR_INT_STS_EN_CMD_TIMEOUT	BIT(0)
#define ERR_INT_STS_EN_ALL_EN		(ERR_INT_STS_EN_ECC | \
					ERR_INT_STS_EN_PREAM | \
					ERR_INT_STS_EN_CRC | \
					ERR_INT_STS_EN_AC | \
					ERR_INT_STS_EN_ADMA)

/* Normal Interrupt Signal Enable Register */
#define INT_STS_SIG_EN			0x14
#define INT_STS_SIG_EN_CA_REQ		BIT(30)
#define INT_STS_SIG_EN_CACHE_RDY	BIT(29)
#define INT_STS_SIG_EN_AC_RDY		BIT(28)
#define INT_STS_SIG_EN_ERR_INT		BIT(15)
#define INT_STS_SIG_EN_DMA_TFR_CMPLT	BIT(7)
#define INT_STS_SIG_EN_BUF_RD_RDY	BIT(5)
#define INT_STS_SIG_EN_BUF_WR_RDY	BIT(4)
#define INT_STS_SIG_EN_DMA_INT		BIT(3)
#define INT_STS_SIG_EN_BLK_GAP		BIT(2)
#define INT_STS_SIG_EN_DAT_CMPLT	BIT(1)
#define INT_STS_SIG_EN_CMD_CMPLT	BIT(0)
#define INT_STS_SIG_EN_ALL_EN		(INT_STS_SIG_EN_AC_RDY | \
					INT_STS_SIG_EN_ERR_INT | \
					INT_STS_SIG_EN_DMA_TFR_CMPLT | \
					INT_STS_SIG_EN_DMA_INT)

/* Error Interrupt Signal Enable Register */
#define ERR_INT_STS_SIG_EN		0x18
#define ERR_INT_STS_SIG_EN_ECC		BIT(19)
#define ERR_INT_STS_SIG_EN_PREAM	BIT(18)
#define ERR_INT_STS_SIG_EN_CRC		BIT(17)
#define ERR_INT_STS_SIG_EN_AC		BIT(16)
#define ERR_INT_STS_SIG_EN_ADMA		BIT(9)
#define ERR_INT_STS_SIG_EN_AUTO_CMD	BIT(8)
#define ERR_INT_STS_SIG_EN_DATA_END	BIT(6)
#define ERR_INT_STS_SIG_EN_DATA_CRC	BIT(5)
#define ERR_INT_STS_SIG_EN_DATA_TIMEOUT BIT(4)
#define ERR_INT_STS_SIG_EN_CMD_IDX	BIT(3)
#define ERR_INT_STS_SIG_EN_CMD_END	BIT(2)
#define ERR_INT_STS_SIG_EN_CMD_CRC	BIT(1)
#define ERR_INT_STS_SIG_EN_CMD_TIMEOUT	BIT(0)
#define ERR_INT_STS_SIG_EN_ALL_EN	(ERR_INT_STS_SIG_EN_ECC | \
					ERR_INT_STS_SIG_EN_PREAM | \
					ERR_INT_STS_SIG_EN_CRC | \
					ERR_INT_STS_SIG_EN_AC | \
					ERR_INT_STS_SIG_EN_ADMA)

/* Transfer Mode register */
#define TFR_MODE			0x1C
	#define TFR_MODE_BUSW_1			0
	#define TFR_MODE_BUSW_2			1
	#define TFR_MODE_BUSW_4			2
	#define TFR_MODE_BUSW_8			3
#define TFR_MODE_DMA_TYPE		BIT(31)
#define TFR_MODE_DMA_KEEP_CSB		BIT(30)
#define TFR_MODE_TO_ENHC		BIT(29)
#define TFR_MODE_PREAM_WITH		BIT(28)
#define TFR_MODE_CSB_DONT_CARE		BIT(27)
#define TFR_MODE_CMD_CNT    		BIT(17)
#define TFR_MODE_DATA_DTR    		BIT(16)
#define TFR_MODE_ADDR_DTR    		BIT(13)
#define TFR_MODE_CMD_DTR    		BIT(10)

#define TFR_MODE_ADDR_CNT_MASK  	OP_ADDR_CNT(0x7)


#define TFR_MODE_SIO_1X_RD_BUS(x)	(((x) & 0x3) << 6)
#define TFR_MODE_MULT_BLK		BIT(5)
#define TFR_MODE_AUTO_CMD(x)		(((x) & 0x3) << 2)
#define TFR_MODE_CNT_EN			BIT(1)
#define TFR_MODE_DMA_EN			BIT(0)
/* share with MAPRD, MAPWR */
	#define OP_DMY_CNT(_len, _dtr, _bw) (((_len * (_dtr + 1)) / (8 / (_bw))) << 21)

	#define OP_DMY(x)		(((x) & 0x3F) << 21)
	#define TFR_MODE_DMY_MASK			(OP_DMY(0x3f))
	#define TFR_MODE_DATA_BUSW_MASK			(OP_DATA_BUSW(0x3))
	#define TFR_MODE_CMD_BUSW_MASK			(OP_CMD_BUSW(0x3))
	#define TFR_MODE_ADDR_BUSW_MASK			(OP_ADDR_BUSW(0x3))

	#define TFR_MODE_ADDR_CNT_MASK			(OP_ADDR_CNT(0x7))


	#define OP_ADDR_CNT(x)		(((x) & 0x7) << 18)
	#define OP_CMD_CNT(x)		(((x) - 1) << 17)
	#define OP_DATA_BUSW(x)		(((x) & 0x3) << 14)
	#define OP_DATA_DTR(x)		(((x) & 0x1) << 16)
	#define OP_ADDR_BUSW(x)		(((x) & 0x3) << 11)
	#define OP_ADDR_DTR(x)		(((x) & 0x1) << 13)
	#define OP_CMD_BUSW(x)		(((x) & 0x3) << 8)
	#define OP_CMD_DTR(x)		(((x) & 1) << 10)
	#define OP_DD_RD		BIT(4)

/* Transfer Control Register */
#define TFR_CTRL			0x20
#define TFR_CTRL_DEV_DIS		BIT(18)
#define TFR_CTRL_IO_END			BIT(16)
#define TFR_CTRL_DEV_ACT		BIT(2)
#define TFR_CTRL_HC_ACT			BIT(1)
#define TFR_CTRL_IO_START		BIT(0)

/* Present State Register */
#define PRES_STS			0x24
#define PRES_STS_ADMA(x)		(((x) & 0x7) << 29)
#define PRES_STS_XSPI_TX(x)		(((x) & 0xF) << 25)
#define PRES_STS_ONFI_TX(x)		(((x) & 0x1F) << 20)
#define PRES_STS_RX_NFULL		BIT(19)
#define PRES_STS_RX_NEMPT		BIT(18)
#define PRES_STS_TX_NFULL		BIT(17)
#define PRES_STS_TX_EMPT		BIT(16)
#define PRES_STS_EMMC_TX(x)		(((x) & 0xF) << 12)
#define PRES_STS_BUF_RD_EN		BIT(11)
#define PRES_STS_BUF_WR_EN		BIT(10)
#define PRES_STS_RD_TFR			BIT(9)
#define PRES_STS_WR_TFR			BIT(8)
#define PRES_STS_DAT_ACT		BIT(2)
#define PRES_STS_CMD_INH_DAT	BIT(1)
#define PRES_STS_CMD_INH_CMD	BIT(0)

/* SDMA Transfer Count Register */
#define SDMA_CNT			0x28
#define SDMA_CNT_TFR_BYTE(x)	(((x) & 0xFFFFFFFF) << 0)

/* SDMA System Address Register */
#define SDMA_ADDR			0x2C
#define SDMA_VAL(x)				(((x) & 0xFFFFFFFF) << 0)

/* ADMA2_System Address Register */
#define ADMA2_ADDR			0x30
#define ADMA2_ADDR_VALUE		(((x) & 0xFFFFFFFF) << 0)

/* ADMA3 System Address Register */
#define ADMA3_ADDR			0x34
#define ADMA3_ADDR_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* Mapping Base Address Register */
#define BASE_MAP_ADDR			0x38
#define BASE_MAP_ADDR_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* Software Reset Register */
#define SW_RST				0x44
#define SW_RST_DAT			BIT(2)
#define SW_RST_CMD			BIT(1)
#define SW_RST_ALL			BIT(0)

/* Timeout Control register */
#define TO_CTRL				0x48
#define TO_CTRL_CA(x)			(((x) & 0xF) << 16)
#define TO_CTRL_DAT(x)			(((x) & 0xF) << 16)

/* Clock Control Register */
#define CLK_CTRL			0x4C
#define CLK_CTRL_SLOW_CLOCK		BIT(31)
#define CLK_CTRL_RX_SS_B(x)		(((x) & 0x1F) << 21)
#define CLK_CTRL_RX_SS_A(x)		(((x) & 0x1F) << 16)
#define CLK_CTRL_PLL_SELECT(x)		(((x) & 0xFFFF) << 0)

/* Cache Control Register */
#define CACHE_CTRL			0x54
#define CACHE_CTRL_DIRTY_LEVEL(x)	(((x) & 0x3) << 30)
#define CACHE_CTRL_LEN_TH(x)		(((x) & 0xff) << 22)
#define CACHE_CTRL_CONT_ADDR		BIT(21)
#define CACHE_CTRL_FETCH_CNT(x)		(((x) & 0x7) << 18)
#define CACHE_CTRL_MST(x)		(((x) & 0xFFFF) << 2)
#define CACHE_CTRL_CLEAN		BIT(1)
#define CACHE_CTRL_INVALID		BIT(0)

/* Capabilities Register */
#define CAP_1				0x58
#define CAP_1_DUAL_CH			BIT(31)
#define CAP_1_XSPI_ITF			BIT(30)
#define CAP_1_ONFI_ITF			BIT(29)
#define CAP_1_EMMC_ITF			BIT(28)
#define CAP_1_MAPPING_MODE		BIT(27)
#define CAP_1_CACHE			BIT(26)
#define CAP_1_ATOMIC			BIT(25)
#define CAP_1_DMA_SLAVE_MODE		BIT(24)
#define CAP_1_DMA_MASTER_MODE		BIT(23)
#define CAP_1_CQE			BIT(22)
#define CAP_1_FIFO_DEPTH(x)		(((x) & 0x3) << 15)
#define CAP_1_SYS_DW(x)			(((x) & 0x3) << 13)
#define CAP_1_LUN_NUM(x)		(((x) & 0xF) << 9)
#define CAP_1_CSB_NUM(x)		(((x) & 0x1FF) << 0)
#define CAP_1_CSB_NUM_MASK		0x1FF
#define CAP_1_CSB_NUM_OFS		0

/* Host Controller Version Register */
#define HC_VER				0x5C
#define HC_VER_VALUE(x)			(((x) & 0xFFFFFFFF) << 0)

/*  RTL Version Register */
#define RTL_VER				0x60
#define RTL_VER_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* Transmit Data 0~3 Register */
#define TXD_REG				0x70
#define TXD(x)				(TXD_REG + ((x) * 4))

/* Receive Data Register */
#define RXD_REG				0x80
#define RXD_VALUE(x)			(((x) & 0xFFFFFFFF) << 0)

/* Send CRC Cycle Register */
#define SEND_CRC_CYC			0x84
#define SEND_CRC_CYC_EN			BIT(0)

/* Block Count Register */
#define BLK_CNT				0x90
#define BLK_CNT_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* Argument Register */
#define ARG_REG				0x94
#define ARG_REG_CMD(x)			(((x) & 0xFFFFFFFF) << 0)

/* Command Register */
#define CMD_REG				0x98
#define CMD_REG_BOOT_BUS(x)		(((x) & 0x7) << 19)
#define CMD_REG_BOOT_TYPE		BIT(18)
#define CMD_REG_BOOT_ACK_EN		BIT(17)
#define CMD_REG_BOOT_EN			BIT(16)
#define CMD_REG_CMD_IDX(x)		(((x) & 3F) << 8)
#define CMD_REG_WR_CRC_STS_EN		BIT(6)
#define CMD_REG_DAT_EN			BIT(5)
#define CMD_REG_CMD_IDX_CHK_EN		BIT(4)
#define CMD_REG_CMD_CRC_CHK_EN		BIT(3)
#define CMD_REG_RSP_SEL(x)		(((x) & 0x3) << 0)

/* Response 1 Register */
#define RSP_1				0x9C
#define RSP_1_VALUE(x)			(((x) & 0xFFFFFFFF) << 0)

/* Response 2 Register */
#define RSP_2				0xA0
#define RSP_2_VALUE(x)			(((x) & 0xFFFFFFFF) << 0)

/* Response 3 Register */
#define RSP_3				0xA4
#define RSP_3_VALUE(x)			(((x) & 0xFFFFFFFF) << 0)

/* Response 4 Register */
#define RSP_4				0xA8
#define RSP_4_1_VALUE(x)		(((x) & 0xff) << 0)
#define RSP_4_0_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* Buffer Data Port register */
#define DATA_REG			0xAC
#define DATA_REG_BUF(x)			(((x) & 0xFFFFFFFF) << 0)

/* Auto CMD Argument Register */
#define AUTO_CMD				0xB0
#define AUTO_CMD_ARGU(x)		(((x) & 0xFFFFFFFF) << 0)

/* Auto CMD Error Status Register */
#define AUTO_CMD_ERR_STS		0xB4
#define AUTO_CMD_ERR_STS_IDX		BIT(4)
#define AUTO_CMD_ERR_STS_END		BIT(3)
#define AUTO_CMD_ERR_STS_CRC		BIT(2)
#define AUTO_CMD_ERR_STS_TIMEOUT	BIT(1)

/* Boot System Address Register */
#define BOOT_SYS_ADDR			0xB8
#define BOOT_SYS_ADDR_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* Block Gap Control Register */
#define BLK_GAP_CTRL			0xBC
#define BLK_GAP_CTRL_CONT_REQ		BIT(1)
#define BLK_GAP_CTRL_STOP_GAP		BIT(0)

/* Device Present Status Register */
#define DEV_CTRL				0xC0
#define DEV_CTRL_TYPE(x)			(((x) & 0x7) << 29)
	#define DEV_CTRL_TYPE_MASK			DEV_CTRL_TYPE(0x7)
	#define DEV_CTRL_TYPE_SPI			DEV_CTRL_TYPE(0)
	#define DEV_CTRL_TYPE_LYBRA			DEV_CTRL_TYPE(1)
	#define DEV_CTRL_TYPE_OCTARAM		DEV_CTRL_TYPE(2)
	#define DEV_CTRL_TYPE_RAWNAND_ONFI	DEV_CTRL_TYPE(4)
	#define DEV_CTRL_TYPE_RAWNAND_JEDEC	DEV_CTRL_TYPE(5)
	#define DEV_CTRL_TYPE_EMMC			DEV_CTRL_TYPE(6)
#define DEV_CTRL_SCLK_SEL(x)		(((x) & 0xF) << 25)
#define DEV_CTRL_SCLK_SEL_MASK		DEV_CTRL_SCLK_SEL(0xF)
#define DEV_CTRL_SCLK_SEL_DIV(x)	(((x >> 1) - 1) << 25)
#define DEV_CTRL_CACHEABLE			BIT(24)
#define DEV_CTRL_WR_PLCY(x)			(((x) & 0x3) << 22)
#define DEV_CTRL_PAGE_SIZE(x)		(((x) & 0x7) << 19)
#define DEV_CTRL_BLK_SIZE(x)		(((x) & 0xFFF) << 7)
#define DEV_CTRL_PRE_DQS_EN			BIT(6)
#define DEV_CTRL_DQS_EN				BIT(5)
#define DEV_CTRL_CRC_EN				BIT(4)
#define DEV_CTRL_CRCB_IN_EN			BIT(3)
#define DEV_CTRL_CRC_CHUNK_SIZE(x)	(((x) & 0x3) << 1)
#define DEV_CTRL_CRCB_OUT_EN		BIT(0)

/* Mapping Read Control Register */
#define MAP_RD_CTRL			0xC4
#define MAP_RD_CTRL_PREAM_EN		BIT(28)
#define MAP_RD_CTRL_SIO_1X_RD(x)	(((x) & 0x3) << 6)

/* Linear/Mapping Write Control Register */
#define MAP_WR_CTRL			0xC8

/* Mapping Command Register */
#define MAP_CMD			0xCC    

/* Top Mapping Address Register */
#define TOP_MAP_ADDR			0xD0
#define TOP_MAP_ADDR_VALUE(x)		(((x) & 0xFFFFFFFF) << 0)

/* General Purpose Inputs and Outputs Register */
#define GPIO_REG			0xD4
#define GPIO_REG_DATA_LEVEL(x)		(((x) & 0xff) << 24)
#define GPIO_REG_RYBYB_LEVE			BIT(23)
#define GPIO_REG_CMD_LEVEL			BIT(22)
#define GPIO_REG_SIO3_EN			BIT(13)
#define GPIO_REG_SIO2_EN			BIT(12)
#define GPIO_REG_SIO3_DRIV_HIGH		BIT(5)
#define GPIO_REG_SIO2_DRIV_HIGH		BIT(4)
#define GPIO_REG_HP_DRIV_HIGH		BIT(3)
#define GPIO_REG_RESTB_DRIV_HIGH	BIT(2)
#define GPIO_REG_HOLDB_DRIV_HIGH	BIT(1)
#define GPIO_REG_WPB_DRIV_HIGH		BIT(0)

/* Auto Calibration Control Register */
#define AC_CTRL				0xD8
#define AC_CTRL_CMD_2(x)			(((x) & 0xff) << 24)
#define AC_CTRL_CMD_1(x)			(((x) & 0xff) << 16)
#define AC_CTRL_WINDOW(x)			(((x) & 0x3) << 14)
#define AC_CTRL_LAZY_DQS_EN			BIT(9)
#define AC_CTRL_LEN_32B_SEL			BIT(8)
#define AC_CTRL_PHY_EN				BIT(6)
#define AC_CTRL_DQS_TEST_EN			BIT(5)
#define AC_CTRL_SIO_ALIG_EN			BIT(4)
#define AC_CTRL_NVDDR_EN			BIT(3)
#define AC_CTRL_SAMPLE_DQS_EN		BIT(2)
#define AC_CTRL_SAMPLE_EN			BIT(1)
#define AC_CTRL_START				BIT(0)

/* Preamble Bit 1 Register */
#define PREAM_1_REG			0xDC
#define PREAM_1_REG_SIO_1(x)		(((x) & 0xFFFF) << 16)
#define PREAM_1_REG_SIO_0(x)		(((x) & 0xFFFF) << 0)

/* Preamble Bit 2 Register */
#define PREAM_2_REG			0xE0
#define PREAM_2_REG_SIO_3(x)		(((x) & 0xFFFF) << 16)
#define PREAM_2_REG_SIO_2(x)		(((x) & 0xFFFF) << 0)

/* Preamble Bit 3 Register */
#define PREAM_3_REG 0xE4
#define PREAM_3_REG_SIO_5(x)		(((x) & 0xFFFF) << 16)
#define PREAM_3_REG_SIO_4(x)		(((x) & 0xFFFF) << 0)

/* Preamble Bit 4 Register */
#define PREAM_4_REG 0xE8
#define PREAM_4_REG_SIO_7(x)		(((x) & 0xFFFF) << 16)
#define PREAM_4_REG_SIO_6(x)		(((x) & 0xFFFF) << 0)

/* Sample Point Adjust Register */
#define SAMPLE_ADJ 			0xEC
#define SAMPLE_ADJ_DQS_IDLY_DOPI(x)	(((x) & 0xff) << 27)
#define SAMPLE_ADJ_DQS_IDLY_SOPI(x)	(((x) & 0xff) << 19)
#define SAMPLE_ADJ_DQS_ODLY(x)		(((x) & 0xff) << 8)
#define SAMPLE_ADJ_POINT_SEL_DDR(x)	(((x) & 0x7) << 3)
#define SAMPLE_ADJ_POINT_SEL_SDR(x)	(((x) & 0x7) << 0)

/* SIO Input Delay 1 Register */
#define SIO_IDLY_1 0xF0
#define SIO_IDLY_1_SIO3(x)		(((x) & 0xff) << 24)
#define SIO_IDLY_1_SIO2(x)		(((x) & 0xff) << 16)
#define SIO_IDLY_1_SIO1(x)		(((x) & 0xff) << 8)
#define SIO_IDLY_1_SIO0(x)		(((x) & 0xff) << 0)
#define SIO_IDLY_1_0123(x)		SIO_IDLY_1_SIO0(x) | \
								SIO_IDLY_1_SIO1(x) | \
								SIO_IDLY_1_SIO2(x) | \
								SIO_IDLY_1_SIO3(x)

/* SIO Input Delay 2 Register */
#define SIO_IDLY_2 0xF4
#define SIO_IDLY_2_SIO4(x)		(((x) & 0xff) << 24)
#define SIO_IDLY_2_SIO5(x)		(((x) & 0xff) << 16)
#define SIO_IDLY_2_SIO6(x)		(((x) & 0xff) << 8)
#define SIO_IDLY_2_SIO7(x)		(((x) & 0xff) << 0)
#define IDLY_CODE_VAL(x, v)		((v) << (((x) % 4) * 8))
#define SIO_IDLY_2_4567(x)		SIO_IDLY_2_SIO4(x) | \
								SIO_IDLY_2_SIO5(x) | \
								SIO_IDLY_2_SIO6(x) | \
								SIO_IDLY_2_SIO7(x)

/* SIO Output Delay 1 Register */
#define SIO_ODLY_1			0xF8
#define SIO_ODLY_1_SIO3(x)		(((x) & 0xff) << 24)
#define SIO_ODLY_1_SIO2(x)		(((x) & 0xff) << 16)
#define SIO_ODLY_1_SIO1(x)		(((x) & 0xff) << 8)
#define SIO_ODLY_1_SIO0(x)		(((x) & 0xff) << 0)

/* SIO Output Delay 2 Register */
#define SIO_ODLY_2			0xFC
#define SIO_ODLY_2_SIO4(x)		(((x) & 0xff) << 24)
#define SIO_ODLY_2_SIO5(x)		(((x) & 0xff) << 16)
#define SIO_ODLY_2_SIO6(x)		(((x) & 0xff) << 8)
#define SIO_ODLY_2_SIO7(x)		(((x) & 0xff) << 0)

#define CONF_HC_XFER_MODE_IO	HC_XFER_MODE_IO
#define CONF_HC_XFER_MODE_MAP	HC_XFER_MODE_MAP
#define CONF_HC_XFER_MODE_DMA	HC_XFER_MODE_DMA

#define UEFC_BASE_ADDRESS 		0x43a00000
#define UEFC_BASE_MAP_ADDR 		0x60000000
#define UEFC_MAP_SIZE			0x00800000
#define UEFC_TOP_MAP_ADDR 		(UEFC_BASE_MAP_ADDR + UEFC_MAP_SIZE)
#define UEFC_BASE_EXT_DDR_ADDR	0x00000000
#define DIR_IN  0
#define DIR_OUT 1
/* Default selection: Channel A, lun 0, Port 0 */
#define UEFC_CH_LUN_PORT 		HC_CTRL_CH_LUN_PORT(A, 0, 0)

#define MXIC_RD32(_reg) \
	(*(volatile uint32_t *)(_reg))

#define MXIC_WR32(_val, _reg) \
	((*(uint32_t *)((_reg))) = (_val))

int mxic_wr32 (uint32_t _val,  uint32_t *_reg) {
	*_reg= (_val);
}

uint32_t swap32(uint32_t val, uint8_t nbytes)
{
	uint32_t ret = 0;
	int n = 0;

	if (nbytes > 4 || nbytes < 1) {
		return -1;
	}

	while (n < nbytes) {
		ret |= ((val >> (n * 8)) & 0xff) << ((nbytes -n -1) * 8);
		n++;
	}

	return ret;
}

#define UPDATE_WRITE(_mask, _value, _reg) \
	MXIC_WR32(((_value) | (MXIC_RD32(_reg) & ~(_mask))), (_reg))

#define MSPI_MAX_FREQ        48000000
#define MSPI_MAX_DEVICE      2
#define MSPI_TIMEOUT_US      1000000
#define PWRCTRL_MAX_WAIT_US  5
#define MSPI_BUSY            BIT(2)

struct mspi_context {
	const struct mspi_dev_id      *owner;

	struct mspi_xfer              xfer;

	mspi_callback_handler_t       callback;
	struct mspi_callback_context  *callback_ctx;
	bool asynchronous;

	struct k_sem lock;
};


