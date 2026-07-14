#include "hal/gpio_ll.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_rom_spiflash.h"
#include "soc/gpio_periph.h"

#define SD_CS   22
#define SD_SCLK 18
#define SD_MOSI 23

#define BTN_B_GPIO      12
#define OTADATA_OFFSET  0x9E000
#define OTADATA_SIZE    0x2000
#define FLASH_SECTOR_SZ 0x1000

void bootloader_after_init(void)
{
    esp_rom_gpio_pad_select_gpio(SD_CS);
    esp_rom_gpio_pad_select_gpio(SD_SCLK);
    esp_rom_gpio_pad_select_gpio(SD_MOSI);

    gpio_ll_output_enable(&GPIO, SD_CS);
    gpio_ll_output_enable(&GPIO, SD_SCLK);
    gpio_ll_output_enable(&GPIO, SD_MOSI);

    gpio_ll_set_level(&GPIO, SD_MOSI, 1);
    gpio_ll_set_level(&GPIO, SD_SCLK, 0);
    gpio_ll_set_level(&GPIO, SD_CS, 0);

    for (int i = 0; i < 80; i++) {
        gpio_ll_set_level(&GPIO, SD_SCLK, 1);
        esp_rom_delay_us(1);
        gpio_ll_set_level(&GPIO, SD_SCLK, 0);
        esp_rom_delay_us(1);
    }

    gpio_ll_set_level(&GPIO, SD_CS, 1);

    /* B 键（GPIO12）按下时擦除 otadata，使 bootloader 回退到 factory（Loader）。
     * 这样用户按住 B 开机即可进入 Loader 菜单重新烧录 ROM。 */
    esp_rom_gpio_pad_select_gpio(BTN_B_GPIO);
    gpio_ll_input_enable(&GPIO, BTN_B_GPIO);
    gpio_ll_pullup_en(&GPIO, BTN_B_GPIO);
    gpio_ll_pulldown_dis(&GPIO, BTN_B_GPIO);
    esp_rom_delay_us(50);

    if (gpio_ll_get_level(&GPIO, BTN_B_GPIO) == 0) {
        for (uint32_t off = 0; off < OTADATA_SIZE; off += FLASH_SECTOR_SZ) {
            esp_rom_spiflash_erase_sector(
                (OTADATA_OFFSET + off) / FLASH_SECTOR_SZ);
        }
    }
}
