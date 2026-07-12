#include "hal/gpio_ll.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_periph.h"

#define SD_CS   22
#define SD_SCLK 18
#define SD_MOSI 23

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
}
