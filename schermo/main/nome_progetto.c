#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define PIN_NUM_SCL      12
#define PIN_NUM_SDA      11
#define PIN_NUM_DC       9
#define PIN_NUM_BLK      8
// Nota: Il pin RES del display è stato staccato dall'ESP32 e collegato fisicamente a 3V3

static spi_device_handle_t spi;

void lcd_cmd(uint8_t cmd) {
    gpio_set_level(PIN_NUM_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi, &t);
}

void lcd_data(uint8_t data) {
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = { .length = 8, .tx_buffer = &data };
    spi_device_polling_transmit(spi, &t);
}

void app_main(void) {
    gpio_reset_pin(PIN_NUM_DC);
    gpio_reset_pin(PIN_NUM_BLK);
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_BLK, GPIO_MODE_OUTPUT);

    // Accendi retroilluminazione
    gpio_set_level(PIN_NUM_BLK, 1);

    // Configurazione SPI a 10MHz
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCL,
        .mosi_io_num = PIN_NUM_SDA,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 240 * 2 + 8
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi);

    // Sequenza estesa ST7789
    lcd_cmd(0x01); // Software Reset
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x11); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x3A); // Interface Pixel Format
    lcd_data(0x05); // 16 bit/pixel (RGB565)

    lcd_cmd(0x36); // Memory Data Access Control
    lcd_data(0x00);

    lcd_cmd(0x21); // Display Inversion On
    lcd_cmd(0x13); // Normal Display Mode On
    
    lcd_cmd(0xE0); // Positive Gamma Control
    uint8_t pgamma[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
    gpio_set_level(PIN_NUM_DC, 1);
    for(int i = 0; i < sizeof(pgamma); i++) {
        lcd_data(pgamma[i]);
    }

    lcd_cmd(0xE1); // Negative Gamma Control
    uint8_t ngamma[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
    for(int i = 0; i < sizeof(ngamma); i++) {
        lcd_data(ngamma[i]);
    }

    lcd_cmd(0x29); // Display ON
    vTaskDelay(pdMS_TO_TICKS(50));

    // Riempi lo schermo di un colore (es. Rosso pieno) per verificare i pixel
    lcd_cmd(0x2A);
    lcd_data(0x00); lcd_data(0x00); lcd_data(0x00); lcd_data(0xEF);
    lcd_cmd(0x2B);
    lcd_data(0x00); lcd_data(0x00); lcd_data(0x00); lcd_data(0xEF);
    lcd_cmd(0x2C);

    gpio_set_level(PIN_NUM_DC, 1);
    uint8_t pixel[2] = {0xF8, 0x00}; // Rosso in RGB565
    for(int i = 0; i < 240 * 240; i++) {
        spi_transaction_t t = { .length = 16, .tx_buffer = pixel };
        spi_device_polling_transmit(spi, &t);
    }
}