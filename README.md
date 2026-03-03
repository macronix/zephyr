## 1. Overview

OOB Overflow Fix: Resolved a potential overflow issue by upgrading the oob_size variable type from uint8_t to uint16_t. This ensures compatibility with modern NAND devices that feature large spare areas (OOB) exceeding 255 bytes.

Memory Optimization: Addressed the BCH heap memory overhead. By implementing an option to store the BCH Look-Up Tables (LUT) in Flash instead of RAM, the runtime heap requirement has been reduced to under 2KB, significantly freeing up SRAM for other system tasks.

## 2. Configuration Guide

### 2.1 Kconfig Integration

The driver behavior can be further refined via prj.conf or menuconfig.

Path: drivers/flash/Kconfig.nand

CONFIG_SPI_NAND_BCH_LUT_STATIC: Stores the 32 KiB BCH LUT in Flash (.rodata) instead of RAM, this reduces the BCH heap size to below 2 KiB.

CONFIG_SPI_NAND_BCH_LUT_ADD_CONFIGURABLE: Enable custom absolute addresses for SPI NAND BCH LUT.

CONFIG_SPI_NAND_POW_LUT_ADDRESS: Absolute address for pow lookup table

CONFIG_SPI_NAND_LOG_LUT_ADDRESS: Absolute address for log lookup table

### 2.2 LUT Binary Integration

The implementation utilizes two binary files containing the pre-calculated 16KB arrays for the Log and Pow tables.

Path:
drivers/flash/pow.bin
drivers/flash/log.bin

## 3. Implementation Details

CMake Build Logic

The build system handles the selection and conversion of binary LUT data into header-compatible formats using a conditional structure in the local CMakeLists.txt.

Path: drivers/flash/CMakeLists.txt

CMake

    if (CONFIG_SPI_NAND_BCH_LUT_STATIC)
        generate_inc_file_for_target(app pow.bin pow.inc)
        generate_inc_file_for_target(app log.bin log.inc)
    endif()
