SECTION_PROLOGUE(.pow_lut_section, ,)
{
    . = ABSOLUTE(CONFIG_SPI_NAND_POW_LUT_ADDRESS);
    KEEP(*(.pow_lut_section))
} GROUP_LINK_IN(FLASH)

SECTION_PROLOGUE(.log_lut_section, ,)
{
    . = ABSOLUTE(CONFIG_SPI_NAND_LOG_LUT_ADDRESS);
    KEEP(*(.log_lut_section))
} GROUP_LINK_IN(FLASH)