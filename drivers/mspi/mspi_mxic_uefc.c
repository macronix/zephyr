/*
 * Copyright (c) 2024, Ambiq Micro Inc. <www.ambiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_mspi_controller

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_instance.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys_clock.h>
#include <zephyr/irq.h>
LOG_MODULE_REGISTER(xlnx_mspi_controller);

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
#define MAP_CMD_RD			0xCC    
#define MAP_CMD_WR			0xCE

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
#define SAMPLE_ADJ_DQS_IDLY_DOPI(x)	(((x) & 0xff) << 24)
#define SAMPLE_ADJ_DQS_IDLY_SOPI(x)	(((x) & 0xff) << 16)
#define SAMPLE_ADJ_DQS_ODLY(x)		(((x) & 0xff) << 8)
#define SAMPLE_ADJ_POINT_SEL_DDR(x)	(((x) & 0x7) << 3)
#define SAMPLE_ADJ_POINT_SEL_SDR(x)	(((x) & 0x7) << 0)

/* SIO Input Delay 1 Register */
#define SIO_IDLY_1 0xF0
#define SIO_IDLY_1_SIO3(x)		(((x) & 0xff) << 24)
#define SIO_IDLY_1_SIO2(x)		(((x) & 0xff) << 16)
#define SIO_IDLY_1_SIO1(x)		(((x) & 0xff) << 8)
#define SIO_IDLY_1_SIO0(x)		(((x) & 0xff) << 0)

/* SIO Input Delay 2 Register */
#define SIO_IDLY_2 0xF4
#define SIO_IDLY_2_SIO4(x)		(((x) & 0xff) << 24)
#define SIO_IDLY_2_SIO5(x)		(((x) & 0xff) << 16)
#define SIO_IDLY_2_SIO6(x)		(((x) & 0xff) << 8)
#define SIO_IDLY_2_SIO7(x)		(((x) & 0xff) << 0)
#define IDLY_CODE_VAL(x, v)		((v) << (((x) % 4) * 8))

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
#define UPDATE_WRITE(_mask, _value, _reg) \
	MXIC_WR32(((_value) | (MXIC_RD32(_reg) & ~(_mask))), (_reg))

static uint32_t mxic_uefc_conf(const struct device *dev);
static int mxic_uefc_hc_setup(const struct device *dev);
static int mxic_uefc_poll_hc_reg(const struct device *dev, uint32_t reg, uint32_t mask);
static int mxic_uefc_io_mode_xfer(const struct device *dev, void *tx, void *rx, uint32_t len,
	uint8_t is_data);
static void mxic_uefc_cs_end(const struct device *dev);
static int mxic_uefc_transceive(const struct device *dev,
			      const struct spi_config *config,
			      const struct spi_buf_set *tx_bufs,
			      const struct spi_buf_set *rx_bufs);
static int mxic_uefc_release(const struct device *dev,
			   const struct spi_config *config);
static int mxic_uefc_init(const struct device *dev);
static void mxic_uefc_cs_start(const struct device *dev);

#define MSPI_MAX_FREQ        48000000
#define MSPI_MAX_DEVICE      2
#define MSPI_TIMEOUT_US      1000000
#define PWRCTRL_MAX_WAIT_US  5
#define MSPI_BUSY            BIT(2)

typedef int (*mspi_ambiq_pwr_func_t)(void);
typedef void (*irq_config_func_t)(void);

struct mspi_context {
	const struct mspi_dev_id      *owner;

	struct mspi_xfer              xfer;

	mspi_callback_handler_t       callback;
	struct mspi_callback_context  *callback_ctx;
	bool asynchronous;

	struct k_sem lock;
};

struct mspi_mxic_config {
	uint32_t                        reg_base;
	uint32_t                        reg_size;

	struct mspi_cfg                 mspicfg;

	const struct pinctrl_dev_config *pcfg;
	irq_config_func_t               irq_cfg_func;

	LOG_INSTANCE_PTR_DECLARE(log);
};

struct mspi_mxic_data {
	void                            *mspiHandle;

	struct mspi_dev_id              *dev_id;
	struct k_mutex                  lock;

	struct mspi_dev_cfg             dev_cfg;
	struct mspi_xip_cfg             xip_cfg;
	struct mspi_scramble_cfg        scramble_cfg;
	uint8_t data_buswidth;
	bool data_dtr;

	mspi_callback_handler_t         cbs[MSPI_BUS_EVENT_MAX];
	struct mspi_callback_context    *cb_ctxs[MSPI_BUS_EVENT_MAX];

	struct mspi_context             ctx;
};

static int mxic_uefc_init(const struct device *dev)
{
	// int ret = 0;
	// const struct mspi_mxic_config *cfg = dev->config;
	// const struct mspi_dt_spec spec = {
	// 	.bus = dev,
	// 	.config = cfg->mspicfg,
	// };

	// mspi_mxic_config(&spec);

	// uint32_t uefc_version = 0;

 	// printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	// DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	// uintptr_t reg_base = DEVICE_MMIO_GET(dev);
 	// printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	// uefc_version = MXIC_RD32(reg_base + INT_STS_SIG_EN);

	// printf ("***[%s], [%s], [%04d], version is %x\r\n", __FILE__, __func__, __LINE__, uefc_version);

	// MXIC_WR32(UEFC_BASE_MAP_ADDR, reg_base + BASE_MAP_ADDR);
	// MXIC_WR32(UEFC_TOP_MAP_ADDR, reg_base + TOP_MAP_ADDR);


	// UPDATE_WRITE(HC_CTRL_CH_LUN_PORT_MASK, UEFC_CH_LUN_PORT, reg_base + HC_CTRL);
	// UPDATE_WRITE(DEV_CTRL_TYPE_MASK | DEV_CTRL_SCLK_SEL_MASK, DEV_CTRL_TYPE_SPI | DEV_CTRL_SCLK_SEL_DIV(4), reg_base + DEV_CTRL);

	// uefc_version = MXIC_RD32(reg_base + HC_VER);

	// printf ("***[%s], [%s], [%04d], version is %x\r\n", __FILE__, __func__, __LINE__, uefc_version);

	// UPDATE_WRITE(HC_CTRL_SIO_SHIFTER(3), HC_CTRL_SIO_SHIFTER(3), reg_base + HC_CTRL);
	
	// MXIC_WR32(CLK_CTRL_RX_SS_A(1) | CLK_CTRL_RX_SS_B(1), reg_base + CLK_CTRL);

	// MXIC_WR32(INT_STS_ALL_CLR, reg_base + INT_STS);
	// MXIC_WR32(INT_STS_EN_ALL_EN, reg_base + INT_STS_EN);
	// MXIC_WR32(INT_STS_SIG_EN_ALL_EN, reg_base + INT_STS_SIG_EN);

	// MXIC_WR32(ERR_INT_STS_ALL_CLR, reg_base + ERR_INT_STS);
	// MXIC_WR32(ERR_INT_STS_EN_ALL_EN, reg_base + ERR_INT_STS_EN);
	// MXIC_WR32(ERR_INT_STS_SIG_EN_ALL_EN, reg_base + ERR_INT_STS_SIG_EN);

	// MXIC_WR32(INT_STS_DMA | INT_STS_EN_DMA_TFR_CMPLT, reg_base + INT_STS_EN);

	// MXIC_WR32(SAMPLE_ADJ_DQS_IDLY_DOPI(0) | SAMPLE_ADJ_POINT_SEL_DDR(0) |
	// 		SAMPLE_ADJ_POINT_SEL_SDR(1), reg_base + SAMPLE_ADJ);

 	// printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);

	// MXIC_WR32(0, reg_base + SIO_IDLY_1);
	// MXIC_WR32(0, reg_base + SIO_IDLY_2);
	// MXIC_WR32(0, reg_base + SIO_ODLY_1);
	// MXIC_WR32(0, reg_base + SIO_ODLY_2);
 	// printf ("***[%s], [%s], [%04d], \r\n", __FILE__, __func__, __LINE__);
	//  k_sem_init(&data->sync_sem, 0, 1);
	//  irq_connect_dynamic(cfg->irq_num, 0, (void *)uefc_mspi_isr, (void *)dev, 0);
	//  irq_enable(cfg->irq_num);

	return 0;
}

static int mxic_uefc_poll_hc_reg(const struct device *dev, uint32_t reg, uint32_t mask)
{
	uint32_t val, n = 10000;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	do {
		val = MXIC_RD32(reg_base + reg) & mask;
		n--;
		k_usleep(1);
	} while (!val && n);
	if (!val) {
		// mxic_pr_err("TIMEOUT! reg(%02Xh) & mask(%08Xh): val(%08Xh)\r\n", reg, mask, val);
		return -1;
	}
	return 0;
}

static void mxic_uefc_cs_start(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	
	/* Enable IO Mode */
	MXIC_WR32(TFR_CTRL_IO_START, reg_base + TFR_CTRL);
	while (TFR_CTRL_IO_START & MXIC_RD32(reg_base + TFR_CTRL));

	/* Enable host controller, reset counter */
	MXIC_WR32(TFR_CTRL_HC_ACT, reg_base + TFR_CTRL);
	while (TFR_CTRL_HC_ACT & MXIC_RD32(reg_base + TFR_CTRL));

	/* Assert CS */
	MXIC_WR32(TFR_CTRL_DEV_ACT, reg_base + TFR_CTRL);
	while (TFR_CTRL_DEV_ACT & MXIC_RD32(reg_base + TFR_CTRL));
}

static void mxic_uefc_cs_end(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	/* De-assert CS */
	MXIC_WR32(TFR_CTRL_DEV_DIS, reg_base + TFR_CTRL);
	while (TFR_CTRL_DEV_DIS & MXIC_RD32(reg_base + TFR_CTRL));

	/* Disable IO Mode */
	MXIC_WR32(TFR_CTRL_IO_END, reg_base + TFR_CTRL);
	while (TFR_CTRL_IO_END & MXIC_RD32(reg_base + TFR_CTRL));
}

static inline void mxic_uefc_err_dessert_cs(const struct device *dev)
{
	mxic_uefc_cs_end(dev);
}

static int mxic_uefc_io_mode_xfer(const struct mspi_xfer *xfer, void *tx, void *rx, uint32_t len,
	uint8_t is_data)
{
	uint32_t nbytes, tmp = 0, ofs = 0;
	uint8_t data_octal_dtr = is_data && data->data_dtr && (8 == data->data_buswidth);

	while (ofs < len) {
		int ret;

		nbytes = len - ofs;
		data = 0xffffffff;

		if (nbytes > 4) {
			nbytes = 4;
		}

		if (tx) {
			memcpy(&data, tx + ofs, nbytes);
			LOG_DBG("tx data: %08X\r\n", data);
		}

		if (data_octal_dtr && (nbytes % 2)) {
			tmp = nbytes;
			nbytes++;
		}

		ret = mxic_uefc_poll_hc_reg(data, PRES_STS, PRES_STS_TX_NFULL);
		if (EXIT_SUCCESS != ret) {
			return ret;
		}

		MXIC_WR32(data, TXD(nbytes % 4));
		ret = mxic_uefc_poll_hc_reg(data, PRES_STS, PRES_STS_RX_NEMPT);
		if (EXIT_SUCCESS != ret) {
			return ret;
		}

		data = MXIC_RD32(RXD_REG);
		if (rx) {
			memcpy(rx + ofs, &data, tmp ? tmp : nbytes);
		}
		LOG_DBG("rx data: %08X\r\n", data);
		ofs += nbytes;
	}

	return EXIT_SUCCESS;
}

static void mspi_mxic_set_line(const struct device *dev, enum mspi_io_mode io_mode,
					  enum mspi_data_rate data_rate)
{
	struct mspi_mxic_data *data = dev->data;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	if (data_rate != MSPI_DATA_RATE_SINGLE) {
		LOG_INST_ERR(cfg->log, "%u, incorrect data rate, only SDR is supported.", __LINE__);
		return -EINVAL;
	}

    uint32_t cmd_bus = 0;
    uint32_t addr_bus = 0;
    uint32_t data_bus = 0;

    bool cmd_ddr  = false;
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

    cmd_bus  = cmd_lines  == 1 ? 0 : cmd_lines == 2 ? 1 : cmd_lines == 4 ? 2 : 3;
    addr_bus = addr_lines == 1 ? 0 : addr_lines == 2 ? 1 : addr_lines == 4 ? 2 : 3;
    data_bus = data_lines == 1 ? 0 : data_lines == 2 ? 1 : data_lines == 4 ? 2 : 3;

	uint32_t conf = MXIC_RD32(reg_base + INT_STS_SIG_EN);

	conf &= ~(
		TFR_MODE_CMD_BUSW_MASK |
		TFR_MODE_ADDR_BUSW_MASK |
		TFR_MODE_DATA_BUSW_MASK |
		TFR_MODE_DATA_DTR |
		TFR_MODE_ADDR_DTR |
		TFR_MODE_CMD_DTR
	);

	conf =	OP_CMD_BUSW(cmd_bus) |
			OP_CMD_DTR(cmd_ddr  ? 1 : 0);

	conf |= OP_ADDR_BUSW(addr_bus) |
			OP_ADDR_DTR(addr_ddr ? 1 : 0);

	conf |= OP_DATA_BUSW(data_bus) |
			OP_DATA_DTR(data_ddr ? 1 : 0);

	data->data_buswidth = data_lines;
	data->data_dtr = data_ddr;

	MXIC_WR32(conf, TFR_MODE);	
}

static inline void mspi_context_release(struct mspi_context *ctx)
{
	ctx->owner = NULL;
	k_sem_give(&ctx->lock);
}

static inline int mspi_context_lock(struct mspi_context *ctx,
				    const struct mspi_dev_id *req,
				    const struct mspi_xfer *xfer,
				    mspi_callback_handler_t callback,
				    struct mspi_callback_context *callback_ctx,
				    bool lockon)
{
	int ret = 1;

	if ((k_sem_count_get(&ctx->lock) == 0) && !lockon &&
	    (ctx->owner == req)) {
		return 0;
	}

	if (k_sem_take(&ctx->lock, K_MSEC(xfer->timeout))) {
		return -EBUSY;
	}

	if (ctx->xfer.async) {
		if ((xfer->tx_dummy == ctx->xfer.tx_dummy) &&
		    (xfer->rx_dummy == ctx->xfer.rx_dummy) &&
		    (xfer->cmd_length == ctx->xfer.cmd_length) &&
		    (xfer->addr_length == ctx->xfer.addr_length)) {
			ret = 0;
		} else if (ctx->callback_ctx) {
				volatile struct mspi_event_data *evt_data;

				evt_data = &ctx->callback_ctx->mspi_evt.evt_data;
				while (evt_data->status != 0) {
				}
				ret = 1;
		} else {
			return -EIO;
		}
	}
	ctx->owner           = req;
	ctx->xfer            = *xfer;
	ctx->callback        = callback;
	ctx->callback_ctx    = callback_ctx;
	return ret;
}

static inline bool mspi_is_inp(const struct device *dev)
{
	struct mspi_mxic_uefc_data *data = dev->data;

	return (k_sem_count_get(&data->ctx.lock) == 0);
}

static inline int mspi_verify_device(const struct device *dev,
				     const struct mspi_dev_id *dev_id)
{
	const struct mspi_mxic_config *cfg = dev->config;
	int device_index = cfg->mspicfg.num_periph;
	int ret = 0;

	for (int i = 0; i < cfg->mspicfg.num_periph; i++) {
		if (dev_id->ce.port == cfg->mspicfg.ce_group[i].port &&
		    dev_id->ce.pin == cfg->mspicfg.ce_group[i].pin &&
		    dev_id->ce.dt_flags == cfg->mspicfg.ce_group[i].dt_flags) {
			device_index = i;
		}
	}

	if (device_index >= cfg->mspicfg.num_periph ||
	    device_index != dev_id->dev_idx) {
		LOG_INST_ERR(cfg->log, "%u, invalid device ID.", __LINE__);
		return -ENODEV;
	}

	return ret;
}

static int mspi_mxic_dev_config(const struct device *dev,
				 const struct mspi_dev_id *dev_id,
				 const enum mspi_dev_cfg_mask param_mask,
				 const struct mspi_dev_cfg *dev_cfg)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	int ret = 0;

	if (data->dev_id != dev_id) {
		if (k_mutex_lock(&data->lock, K_MSEC(CONFIG_MSPI_COMPLETION_TIMEOUT_TOLERANCE))) {
			LOG_INST_ERR(cfg->log, "%u, fail to gain controller access.", __LINE__);
			return -EBUSY;
		}

		ret = mspi_verify_device(dev, dev_id);
		if (ret) {
			goto e_return;
		}
	}

	if (mspi_is_inp(dev)) {
		ret = -EBUSY;
		goto e_return;
	}

	if (param_mask == MSPI_DEVICE_CONFIG_NONE &&
	    !cfg->mspicfg.sw_multi_periph) {
		/* Do nothing except obtaining the controller lock */
		data->dev_id = (struct mspi_dev_id *)dev_id;
		return ret;
	} else if (param_mask != MSPI_DEVICE_CONFIG_ALL) {
		if (data->dev_id != dev_id) {
			LOG_INST_ERR(cfg->log, "%u, config failed, must be the same device.",
				     __LINE__);
			ret = -ENOTSUP;
			goto e_return;
		}

		if ((param_mask & (~(MSPI_DEVICE_CONFIG_FREQUENCY |
				     MSPI_DEVICE_CONFIG_IO_MODE |
				     MSPI_DEVICE_CONFIG_CE_NUM |
				     MSPI_DEVICE_CONFIG_DATA_RATE |
				     MSPI_DEVICE_CONFIG_CMD_LEN |
				     MSPI_DEVICE_CONFIG_ADDR_LEN)))) {
			LOG_INST_ERR(cfg->log, "%u, config type not supported.", __LINE__);
			ret = -ENOTSUP;
			goto e_return;
		}

		if (param_mask & MSPI_DEVICE_CONFIG_FREQUENCY) {
			UPDATE_WRITE(DEV_CTRL_SCLK_SEL_MASK, DEV_CTRL_SCLK_SEL_DIV(4), reg_base + DEV_CTRL);
			data->dev_cfg.freq = dev_cfg->freq;
		}

		if ((param_mask & MSPI_DEVICE_CONFIG_IO_MODE) ||
		    (param_mask & MSPI_DEVICE_CONFIG_CE_NUM) ||
		    (param_mask & MSPI_DEVICE_CONFIG_DATA_RATE)) {
			mspi_mxic_set_line(dev, dev_cfg->io_mode, dev_cfg->data_rate);

			data->dev_cfg.freq      = dev_cfg->io_mode;
			data->dev_cfg.data_rate = dev_cfg->data_rate;
		}

		if (param_mask & MSPI_DEVICE_CONFIG_CMD_LEN) {
			if (dev_cfg->cmd_length > MXIC_UEFC_CMD_LENGTH ||
			    dev_cfg->cmd_length == 0) {
				LOG_INST_ERR(cfg->log, "%u, invalid cmd_length.", __LINE__);
				ret = -ENOTSUP;
				goto e_return;
			}

			UPDATE_WRITE(TFR_MODE_CMD_CNT, OP_CMD_CNT(dev_cfg->cmd_length) , TFR_MODE);

			data->dev_cfg.cmd_length = dev_cfg->cmd_length;
		}

		if (param_mask & MSPI_DEVICE_CONFIG_ADDR_LEN) {
			if (dev_cfg->addr_length > MXIC_UEFC_ADDR_LENGTH ||
			    dev_cfg->addr_length == 0) {
				LOG_INST_ERR(cfg->log, "%u, invalid addr_length.", __LINE__);
				ret = -ENOTSUP;
				goto e_return;
			}

			UPDATE_WRITE(TFR_MODE_ADDR_CNT_MASK, OP_ADDR_CNT(dev_cfg->addr_length), TFR_CTRL);

			data->dev_cfg.addr_length = dev_cfg->addr_length;
		}
	} else {
		if (data->dev_id != dev_id) {
			ret = pinctrl_apply_state(cfg->pcfg,
						  PINCTRL_STATE_PRIV_START + dev_id->dev_idx);
			if (ret) {
				goto e_return;
			}
		}

		if (memcmp(&data->dev_cfg, dev_cfg, sizeof(struct mspi_dev_cfg)) == 0) {
			/** Nothing to config */
			data->dev_id = (struct mspi_dev_id *)dev_id;
			return ret;
		}

		if (dev_cfg->endian != MSPI_XFER_LITTLE_ENDIAN) {
			LOG_INST_ERR(cfg->log, "%u, only support MSB first.", __LINE__);
			ret = -ENOTSUP;
			goto e_return;
		}

		if (dev_cfg->dqs_enable && !cfg->mspicfg.dqs_support) {
			LOG_INST_ERR(cfg->log, "%u, only support non-DQS mode.", __LINE__);
			ret = -ENOTSUP;
			goto e_return;
		}

		mspi_mxic_set_line(dev_cfg->io_mode, dev_cfg->data_rate);

		UPDATE_WRITE(TFR_MODE_CMD_CNT, OP_CMD_CNT(dev_cfg->cmd_length) , TFR_MODE);
		UPDATE_WRITE(TFR_MODE_ADDR_CNT_MASK, OP_ADDR_CNT(dev_cfg->addr_length), TFR_MODE);

		data->dev_cfg = *dev_cfg;
		data->dev_id = (struct mspi_dev_id *)dev_id;
	}

	return ret;

e_return:
	k_mutex_unlock(&data->lock);
	return ret;
}

static void uefc_mspi_isr(const struct device *dev)
{
	const struct uefc_mspi_config *cfg = dev->config;
	struct uefc_mspi_data *data = dev->data;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint32_t int_sts = MXIC_RD32(reg_base + INT_STS);

	if (int_sts & INT_STS_DMA_TFR_CMPLT) {
		MXIC_WR32(reg_base + INT_STS, INT_STS_DMA_TFR_CMPLT); // write-1-clear
		data->xfer_pending = false;

		if (data->callback) {
			data->callback(dev, data->cb_data, 0); // success
		} else {
			k_sem_give(&data->sync_sem);
		}
	}
}

static int mspi_mxic_register_callback(const struct device *dev,
					const struct mspi_dev_id *dev_id,
					const enum mspi_bus_event evt_type,
					mspi_callback_handler_t cb,
					struct mspi_callback_context *ctx)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;

	if (mspi_is_inp(dev)) {
		return -EBUSY;
	}

	if (dev_id != data->dev_id) {
		LOG_INST_ERR(cfg->log, "%u, dev_id don't match.", __LINE__);
		return -ESTALE;
	}

	if (evt_type != MSPI_BUS_XFER_COMPLETE) {
		LOG_INST_ERR(cfg->log, "%u, callback types not supported.", __LINE__);
		return -ENOTSUP;
	}

	data->cbs[evt_type] = cb;
	data->cb_ctxs[evt_type] = ctx;
	return 0;
}

// static int mspi_dma_transceive(const struct device *dev,
// 			       const struct mspi_xfer *xfer,
// 			       mspi_callback_handler_t cb,
// 			       struct mspi_callback_context *cb_ctx)
// {
// 	const struct mspi_mxic_config *cfg = dev->config;
// 	struct mspi_mxic_data *data = dev->data;
// 	struct mspi_context *ctx = &data->ctx;
// 	int ret = 0;
// 	int cfg_flag = 0;

// 	if (xfer->num_packet == 0 ||
// 	    !xfer->packets ||
// 	    xfer->timeout > CONFIG_MSPI_COMPLETION_TIMEOUT_TOLERANCE) {
// 		return -EFAULT;
// 	}

// 	cfg_flag = mspi_context_lock(ctx, data->dev_id, xfer, cb, cb_ctx, true);
// 	/** For async, user must make sure when cfg_flag = 0 the dummy and instr addr length
// 	 * in mspi_xfer of the two calls are the same if the first one has not finished yet.
// 	 */
// 	if (cfg_flag) {
// 		if (cfg_flag == 1) {
// 			ret = mspi_xfer_config(dev, xfer);
// 			if (ret) {
// 				goto dma_err;
// 			}
// 		} else {
// 			ret = cfg_flag;
// 			goto dma_err;
// 		}
// 	}

// 	if (HC_XFER_MODE_DMA == xfer->pkts->xfer_mode) {
// 		conf |= TFR_MODE_DMA_EN;
// 	}

// 	// /* Clear DMA_TFR_CMPLT & DMA_INIT in REG_INT_STS */
// 	// MXIC_WR32(INT_STS_DMA_TFR_CMPLT | INT_STS_DMA_INT, INT_STS);

// 	mxic_uefc_cs_start(xfer);

// 	/* Set up command  */
// 	if (xfer->cmd_length) {
// 		ret = mxic_uefc_io_mode_xfer(xfer, (uint8_t *)&xfer->packets->cmd_length, 0, xfer->packets->cmd_length,
// 				0);
// 		if (EXIT_SUCCESS != ret) {
// 			mxic_uefc_err_dessert_cs(xfer);
// 			return ret;
// 		}
// 	}

// 	/* Set up address */
// 	if (xfer->addr_length) {
// 		ret = mxic_uefc_io_mode_xfer(xfer, (uint8_t *)&addr, 0, xfer->packets->addr_length, 0);
// 		if (EXIT_SUCCESS != ret) {
// 			mxic_uefc_err_dessert_cs(xfer);
// 		}
// 	}

// 	uint32_t dummy_length = MSPI_TX == xfer->packets->dir ? xfer->tx_dummy : xfer->rx_dummy

// 	/* Setup dummy: dummy's bus width and DTR are determined by the data */
// 	if (dummy_length) {
// 		dummy_len = (dummy_length * (data->data_dtr + 1)) / (8 / (data->data_buswidth));
// 		ret = mxic_uefc_io_mode_xfer(xfer, 0, 0, dummy_len, 0);
// 		if (EXIT_SUCCESS != ret) {
// 			mxic_uefc_err_dessert_cs(xfer);
// 			return ret;
// 		}
// 	}

// 	/* Set up read/write Data */
// 	if (xfer->packets->data_buf) {
// 		ret = mxic_uefc_io_mode_xfer(xfer,
// 			MSPI_TX == xfer->packets->dir  ? xfer->packets->data_buf : 0,
// 			MSPI_RX == xfer->packets->dir ? xfer->packets->data_buf : 0,
// 			xfer->packets->data_buf, 1);
// 		if (EXIT_SUCCESS != ret) {
// 			mxic_uefc_err_dessert_cs(xfer);
// 			return ret;
// 		}
// 	}

// 	MXIC_WR32(xfer->pkts->data.len, SDMA_CNT);
// 	MXIC_WR32((uint32_t)xfer->pkts->data.buf, SDMA_ADDR);

// 	const struct mspi_xfer_packet *packet;

// 	packet                     = &ctx->xfer.packets[packet_idx];

// 	if (ctx->xfer.async) {

// 		if (ctx->callback && packet->cb_mask == MSPI_BUS_XFER_COMPLETE_CB) {
// 			ctx->callback_ctx->mspi_evt.evt_type = MSPI_BUS_XFER_COMPLETE;
// 			ctx->callback_ctx->mspi_evt.evt_data.dev = dev;
// 			ctx->callback_ctx->mspi_evt.evt_data.dev_id = data->ctx.owner;
// 			ctx->callback_ctx->mspi_evt.evt_data.packet = packet;
// 			ctx->callback_ctx->mspi_evt.evt_data.packet_idx = packet_idx;
// 			ctx->callback_ctx->mspi_evt.evt_data.status = ~0;
// 		}

// 		if (packet->cb_mask == MSPI_BUS_XFER_COMPLETE_CB) {
// 		}

// 	} else {

// 	}
// 	if (ret) {
// 		if (ret == AM_HAL_STATUS_OUT_OF_RANGE) {
// 			ret = -ENOMEM;
// 		} else {
// 			ret = -EIO;
// 		}
// 		goto dma_err;
// 	}

// 	if (!ctx->xfer.async) {
// 		do {
// 			reg_int_sts = MXIC_RD32(INT_STS);

// 			if (INT_STS_DMA_INT & reg_int_sts) {
// 				MXIC_WR32(INT_STS_DMA_INT, INT_STS);
// 				MXIC_WR32(MXIC_RD32(SDMA_ADDR), SDMA_ADDR);
// 			}

// 		} while (!(INT_STS_DMA_TFR_CMPLT & reg_int_sts));
// 	}

// dma_err:
// 	mspi_context_release(ctx);
// 	return ret;
// }

static int mspi_pio_prepare(const struct device *dev)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	const struct mspi_xfer *xfer = &data->ctx.xfer;
	int ret = 0;
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	uint32_t conf = MXIC_RD32(reg_base + INT_STS_SIG_EN);

	conf &= ~(
		TFR_MODE_ADDR_CNT_MASK |
		TFR_MODE_CMD_CNT |
		TFR_MODE_DMY_MASK |
		OP_DD_RD
	);

	conf |= OP_CMD_CNT(xfer->cmd_length) |
			OP_ADDR_CNT(xfer->addr_length);

	conf |= OP_DMY_CNT(xfer->pkts->dummy.len, data->data_dtr, data->data_buswidth);

	conf |= (DIR_IN == xfer->dir ? OP_DD_RD: 0);

	MXIC_WR32(conf, TFR_MODE);	

	return ret;
}

static int mspi_pio_transceive(const struct device *dev,
			       const struct mspi_xfer *xfer,
			       mspi_callback_handler_t cb,
			       struct mspi_callback_context *cb_ctx)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	struct mspi_context *ctx = &data->ctx;
	const struct mspi_xfer_packet *packet;
	uint32_t packet_idx;
	int ret = 0;
	int cfg_flag = 0;

	if (xfer->num_packet == 0 ||
	    !xfer->packets) {
		return -EFAULT;
	}

	cfg_flag = mspi_context_lock(ctx, data->dev_id, xfer, cb, cb_ctx, true);
	/** For async, user must make sure when cfg_flag = 0 the dummy and instr addr length
	 * in mspi_xfer of the two calls are the same if the first one has not finished yet.
	 */
	if (cfg_flag) {
		if (cfg_flag == 1) {
			ret = mspi_pio_prepare(dev);
			if (ret) {
				goto pio_err;
			}
		} else {
			ret = cfg_flag;
			goto pio_err;
		}
	}

	mxic_uefc_cs_start(dev);

	/* Set up command  */
	if (xfer->cmd_length) {
		ret = mxic_uefc_io_mode_xfer(xfer, (uint8_t *)&xfer->packets->cmd_length, 0, xfer->packets->cmd_length,
				0);
		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	/* Set up address */
	if (xfer->addr_length) {
		ret = mxic_uefc_io_mode_xfer(xfer, (uint8_t *)&addr, 0, xfer->packets->addr_length, 0);
		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
		}
	}

	uint32_t dummy_length = MSPI_TX == xfer->packets->dir ? xfer->tx_dummy : xfer->rx_dummy;

	/* Setup dummy: dummy's bus width and DTR are determined by the data */
	if (dummy_length) {
		dummy_len = (dummy_length * (data->data_dtr + 1)) / (8 / (data->data_buswidth));
		ret = mxic_uefc_io_mode_xfer(xfer, 0, 0, dummy_len, 0);
		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	/* Set up read/write Data */
	if (xfer->packets->data_buf) {
		ret = mxic_uefc_io_mode_xfer(xfer,
			MSPI_TX == xfer->packets->dir  ? xfer->packets->data_buf : 0,
			MSPI_RX == xfer->packets->dir ? xfer->packets->data_buf : 0,
			xfer->packets->data_buf, 1);
		if (EXIT_SUCCESS != ret) {
			mxic_uefc_err_dessert_cs(dev);
			return ret;
		}
	}

	mxic_uefc_cs_end(dev);

pio_err:
	mspi_context_release(ctx);
	return ret;
}

// static int mspi_mxic_xip_config(const struct device *dev,
// 				 const struct mspi_dev_id *dev_id,
// 				 const struct mspi_xip_cfg *xip_cfg)
// {
// 	const struct mspi_mxic_config *cfg = dev->config;
// 	struct mspi_mxic_data *data = dev->data;
// 	int ret = 0;

// 	if (dev_id != data->dev_id) {
// 		LOG_INST_ERR(cfg->log, "%u, dev_id don't match.", __LINE__);
// 		return -ESTALE;
// 	}

// 	uint32_t addr = (uint32_t)xfer->packets->address;

// 	/* End IO Mode */
// 	MXIC_WR32(TFR_CTRL_IO_END, TFR_CTRL);

// 	if (DIR_IN == xfer->packets->dir) {
// 		MXIC_WR32(mxic_uefc_conf(xfer), MAP_RD_CTRL);
// 		MXIC_WR32(xfer->packets->cmd, MAP_CMD);
// 	} else {
// 		MXIC_WR32(mxic_uefc_conf(xfer), MAP_WR_CTRL);
// 		MXIC_WR32(xfer->packets->cmd << 16, MAP_CMD);
// 	}

// 	data->xip_cfg = *xip_cfg;
// 	return ret;
// }

static int mspi_mxic_transceive(const struct device *dev,
				 const struct mspi_dev_id *dev_id,
				 const struct mspi_xfer *xfer)
{
	const struct mspi_mxic_config *cfg = dev->config;
	struct mspi_mxic_data *data = dev->data;
	mspi_callback_handler_t cb = NULL;
	struct mspi_callback_context *cb_ctx = NULL;

	if (dev_id != data->dev_id) {
		LOG_INST_ERR(cfg->log, "%u, dev_id don't match.", __LINE__);
		return -ESTALE;
	}

	if (xfer->async) {
		cb = data->cbs[MSPI_BUS_XFER_COMPLETE];
		cb_ctx = data->cb_ctxs[MSPI_BUS_XFER_COMPLETE];
	}

	if (xfer->xfer_mode == MSPI_PIO) {
		return mspi_pio_transceive(controller, xfer, cb, cb_ctx);
	} else if (xfer->xfer_mode == MSPI_DMA) {
		return mspi_dma_transceive(controller, xfer, cb, cb_ctx);
	} else {
		return -EIO;
	}
}

static int mspi_mxic_config(const struct mspi_dt_spec *spec)
{
	ARG_UNUSED(spec);

	return -ENOTSUP;
}

static struct mspi_driver_api mspi_mxic_driver_api = {
	.config                = mspi_mxic_config,
	.dev_config            = mspi_mxic_dev_config,
	//.get_channel_status    = mspi_mxic_get_channel_status,
	.register_callback     = mspi_mxic_register_callback,
	.transceive            = mspi_mxic_transceive,
};

// #define MSPI_CONFIG(n)                                                                           \
// 	{                                                                                        \
// 		.channel_num           = (DT_INST_REG_ADDR(n) - MSPI0_BASE) /                    \
// 					 (DT_INST_REG_SIZE(n) * 4),                              \
// 		.op_mode               = MSPI_OP_MODE_CONTROLLER,                                \
// 		.duplex                = MSPI_HALF_DUPLEX,                                       \
// 		.max_freq              = MSPI_MAX_FREQ,                                          \
// 		.dqs_support           = false,                                                  \
// 		.num_periph            = DT_INST_CHILD_NUM(n),                                   \
// 		.sw_multi_periph       = DT_INST_PROP(n, software_multiperipheral),              \
// 	}


// #define XLNX_MSPI_DEFINE(n)                                                                     \
// 	LOG_INSTANCE_REGISTER(DT_DRV_INST(n), mspi##n, CONFIG_MSPI_LOG_LEVEL);                   \
// 	MSPI_PINCTRL_DT_DEFINE(DT_DRV_INST(n));                                                  \
// 	static void mspi_ambiq_irq_cfg_func_##n(void)                                            \
// 	{                                                                                        \
// 		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),                           \
// 		mspi_ambiq_isr, DEVICE_DT_INST_GET(n), 0);                                       \
// 		irq_enable(DT_INST_IRQN(n));                                                     \
// 	}                                                                                        \
// 	static uint32_t mspi_ambiq_cmdq##n[DT_INST_PROP_OR(n, cmdq_buffer_size, 1024) / 4]       \
// 	__attribute__((section(DT_INST_PROP_OR(n, cmdq_buffer_location, ".mspi_buff"))));        \
// 	static struct gpio_dt_spec ce_gpios##n[] = MSPI_CE_GPIOS_DT_SPEC_INST_GET(n);            \
// 	static struct mspi_mxic_data mspi_mxic_data##n = {                                     \
// 		.mspiHandle            = NULL,                                                   \
// 		.hal_dev_cfg           = MSPI_HAL_DEVICE_CONFIG(n, mspi_ambiq_cmdq##n,           \
// 					 DT_INST_PROP_OR(n, cmdq_buffer_size, 1024)),            \
// 		.dev_id                = 0,                                                      \
// 		.lock                  = Z_MUTEX_INITIALIZER(mspi_mxic_data##n.lock),           \
// 		.dev_cfg               = {0},                                                    \
// 		.xip_cfg               = {0},                                                    \
// 		.scramble_cfg          = {0},                                                    \
// 		.cbs                   = {0},                                                    \
// 		.cb_ctxs               = {0},                                                    \
// 		.ctx.lock              = Z_SEM_INITIALIZER(mspi_mxic_data##n.ctx.lock, 0, 1),   \
// 		.ctx.callback          = 0,                                                      \
// 		.ctx.callback_ctx      = 0,                                                      \
// 	};                                                                                       \
// 	static const struct mspi_mxic_config mspi_mxic_config##n = {                           \
// 		.reg_base              = DT_INST_REG_ADDR(n),                                    \
// 		.reg_size              = DT_INST_REG_SIZE(n),                                    \
// 		.mspicfg               = MSPI_CONFIG(n),                                         \
// 		.mspicfg.ce_group      = (struct gpio_dt_spec *)ce_gpios##n,                     \
// 		.mspicfg.num_ce_gpios  = ARRAY_SIZE(ce_gpios##n),                                \
// 		.mspicfg.re_init       = false,                                                  \
// 		.pcfg                  = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                      \
// 		.irq_cfg_func          = mspi_ambiq_irq_cfg_func_##n,                            \
// 		LOG_INSTANCE_PTR_INIT(log, DT_DRV_INST(n), mspi##n)                              \
// 	};                                                                                       \
// 	PM_DEVICE_DT_INST_DEFINE(n, mspi_mxic_pm_action);                                       \
// 	DEVICE_DT_INST_DEFINE(n,                                                                 \
// 			      mspi_mxic_init,                                                   \
// 			      NULL,                                          \
// 			      NULL,                                               \
// 			      NULL,                                             \
// 			      POST_KERNEL,                                                       \
// 			      CONFIG_MSPI_INIT_PRIORITY,                                         \
// 			     NULL);

// DT_INST_FOREACH_STATUS_OKAY(XLNX_MSPI_DEFINE)
static const struct mspi_mxic_config  mspi_mxic_config_0 {
	                        .reg_base = 0;
};

static const struct mspi_mxic_data  mspi_mxic_data_0;

	DEVICE_DT_INST_DEFINE(0,                                                                 
			      &mxic_uefc_init,                                                   
			      NULL,                                          
			      &mspi_mxic_data_0,                                               
			      &mspi_mxic_config_0,                                             
			      POST_KERNEL,                                                       
			      CONFIG_MSPI_INIT_PRIORITY,                                         
			     &mspi_mxic_driver_api);
