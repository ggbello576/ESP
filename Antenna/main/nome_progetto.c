#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "CC1101_SNIFFER";

// Pinout ESP32-S3 -> CC1101
#define PIN_MISO    13
#define PIN_MOSI    11
#define PIN_SCK     12
#define PIN_CSN     10
#define PIN_GDO0    15
#define PIN_GDO2    16

// Command Strobes CC1101
#define CC1101_SRES         0x30
#define CC1101_SRX          0x34
#define CC1101_SIDLE        0x36

// Registri di Configurazione CC1101
#define CC1101_IOCFG0       0x02
#define CC1101_PKTCTRL0     0x08
#define CC1101_FSCTRL1      0x0B
#define CC1101_FREQ2        0x0D
#define CC1101_FREQ1        0x0E
#define CC1101_FREQ0        0x0F
#define CC1101_MDMCFG4      0x10
#define CC1101_MDMCFG3      0x11
#define CC1101_MDMCFG2      0x12
#define CC1101_MDMCFG1      0x13
#define CC1101_MDMCFG0      0x14
#define CC1101_AGCCTRL2     0x1B
#define CC1101_AGCCTRL1     0x1C
#define CC1101_AGCCTRL0     0x1D
#define CC1101_FSCAL3       0x23
#define CC1101_FSCAL2       0x24
#define CC1101_FSCAL1       0x25
#define CC1101_FSCAL0       0x26

// Registri di Stato
#define CC1101_VERSION      0x31
#define CC1101_RSSI         0x34
#define CC1101_MARCSTATE    0x35
#define CC1101_READ_BURST   0xC0

// Parametri buffer cattura impulsi
#define MAX_PULSES          256
#define MIN_PULSES          24      // Almeno 12 bit (24 fronti) per considerare valido il pacchetto
#define NOISE_MIN_US        120     // Impulsi sotto i 120 us vengono scartati come rumore AGC
#define PACKET_GAP_US       4000    // Pausa > 4 ms indica fine pacchetto / sync gap

static spi_device_handle_t cc1101_spi;

// Variabili condivise tra Interrupt (ISR) e Task principale
static volatile int32_t pulses[MAX_PULSES];
static volatile uint16_t pulse_count = 0;
static volatile int64_t last_edge_time = 0;
static volatile bool packet_ready = false;

static void cc1101_strobe(uint8_t strobe) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &strobe,
    };
    ESP_ERROR_CHECK(spi_device_transmit(cc1101_spi, &t));
}

static void cc1101_write_reg(uint8_t addr, uint8_t value) {
    uint8_t tx_buf[2] = { addr, value };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(cc1101_spi, &t));
}

static uint8_t cc1101_read_status(uint8_t addr) {
    uint8_t tx_buf[2] = { (uint8_t)(addr | CC1101_READ_BURST), 0x00 };
    uint8_t rx_buf[2] = { 0, 0 };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(cc1101_spi, &t));
    return rx_buf[1];
}

static void cc1101_init_433_ook(void) {
    cc1101_strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));

    // GDO0 = Serial Data Output asincrono
    cc1101_write_reg(CC1101_IOCFG0,   0x0D);
    cc1101_write_reg(CC1101_PKTCTRL0, 0x32);
    cc1101_write_reg(CC1101_FSCTRL1,  0x06);

    // 433.920 MHz (quarzo 26 MHz)
    cc1101_write_reg(CC1101_FREQ2,    0x10);
    cc1101_write_reg(CC1101_FREQ1,    0xB0);
    cc1101_write_reg(CC1101_FREQ0,    0xA4);

    // Modulazione ASK/OOK, filtro banda ~203 kHz
    cc1101_write_reg(CC1101_MDMCFG4,  0x87);
    cc1101_write_reg(CC1101_MDMCFG3,  0x83);
    cc1101_write_reg(CC1101_MDMCFG2,  0x30);
    cc1101_write_reg(CC1101_MDMCFG1,  0x22);
    cc1101_write_reg(CC1101_MDMCFG0,  0xF8);

    // AGC tarato per impulsi OOK
    cc1101_write_reg(CC1101_AGCCTRL2, 0x03);
    cc1101_write_reg(CC1101_AGCCTRL1, 0x00);
    cc1101_write_reg(CC1101_AGCCTRL0, 0x91);

    // Calibrazione PLL
    cc1101_write_reg(CC1101_FSCAL3,   0xE9);
    cc1101_write_reg(CC1101_FSCAL2,   0x2A);
    cc1101_write_reg(CC1101_FSCAL1,   0x00);
    cc1101_write_reg(CC1101_FSCAL0,   0x1F);

    // Entra in ricezione
    cc1101_strobe(CC1101_SRX);
    vTaskDelay(pdMS_TO_TICKS(5));
}

// Interrupt Hardware su GPIO 15 (eseguito in RAM veloce IRAM)
static void IRAM_ATTR gdo0_isr_handler(void *arg) {
    if (packet_ready) {
        return; // Attendi che il task main finisca di stampare il pacchetto
    }

    int64_t now = esp_timer_get_time();
    int64_t dt = now - last_edge_time;
    last_edge_time = now;

    // Se ora GDO0 è 0, l'impulso appena terminato era ALTO (+); viceversa era BASSO (-)
    int current_level = gpio_get_level(PIN_GDO0);
    int ended_level = !current_level;

    // 1. Pausa lunga (> 4 ms): fine pacchetto o intervallo tra due ripetizioni
    if (dt > PACKET_GAP_US) {
        if (pulse_count >= MIN_PULSES) {
            if (pulse_count < MAX_PULSES) {
                pulses[pulse_count++] = ended_level ? (int32_t)dt : -(int32_t)dt;
            }
            packet_ready = true;
        } else {
            pulse_count = 0; // Scarta frammenti troppo corti
        }
        return;
    }

    // 2. Micro-impulso di rumore termico (< 120 us): resetta la cattura
    if (dt < NOISE_MIN_US) {
        pulse_count = 0;
        return;
    }

    // 3. Impulso valido: salva durata con segno in RAM
    if (pulse_count < MAX_PULSES) {
        pulses[pulse_count++] = ended_level ? (int32_t)dt : -(int32_t)dt;
    } else {
        packet_ready = true;
    }
}

// Tenta di decodificare impulsi PWM (corto = 0, lungo = 1)
static void decode_ook_pwm(const int32_t *buf, uint16_t count) {
    int32_t min_high = INT32_MAX;
    int32_t max_high = 0;

    for (uint16_t i = 0; i < count; i++) {
        if (buf[i] > 0 && buf[i] < PACKET_GAP_US) {
            if (buf[i] < min_high) min_high = buf[i];
            if (buf[i] > max_high) max_high = buf[i];
        }
    }

    // Verifica che ci sia una chiara distinzione tra impulsi corti e lunghi
    if (min_high == INT32_MAX || max_high < (min_high * 1.6)) {
        printf("Decodifica PWM: pattern non standard (solo RAW)\n");
        return;
    }

    int32_t threshold = (min_high + max_high) / 2;
    char bits[MAX_PULSES];
    uint16_t bit_len = 0;
    uint64_t hex_val = 0;

    for (uint16_t i = 0; i < count; i++) {
        if (buf[i] > 0 && buf[i] < PACKET_GAP_US) {
            char b = (buf[i] > threshold) ? '1' : '0';
            bits[bit_len++] = b;
            if (bit_len <= 64) {
                hex_val = (hex_val << 1) | (b == '1' ? 1ULL : 0ULL);
            }
        }
    }
    bits[bit_len] = '\0';

    printf("Bit (%u): %s\n", bit_len, bits);
    if (bit_len <= 64) {
        printf("HEX: 0x%llX (soglia: %" PRId32 " us)\n", hex_val, threshold);
    }
}

void app_main(void) {
    // 1. Inizializzazione bus SPI
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CSN,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &cc1101_spi));

    // 2. Avvio CC1101 sui 433.92 MHz OOK
    cc1101_init_433_ook();

    uint8_t ver = cc1101_read_status(CC1101_VERSION);
    uint8_t state = cc1101_read_status(CC1101_MARCSTATE) & 0x1F;
    ESP_LOGI(TAG, "Sniffer pronto | VER: 0x%02X | STATE: 0x%02X | Ascolto 433.92 MHz...", ver, state);

    // 3. Configurazione Interrupt Hardware su GPIO 15 (GDO0)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_GDO0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_GDO0, gdo0_isr_handler, NULL));

    int32_t local_pulses[MAX_PULSES];

    while (1) {
        // Flush di sicurezza se arriva un pacchetto singolo isolato seguito da silenzio > 10 ms
        if (!packet_ready && pulse_count >= MIN_PULSES) {
            if ((esp_timer_get_time() - last_edge_time) > 10000) {
                packet_ready = true;
            }
        }

        if (packet_ready) {
            uint16_t count = pulse_count;
            for (uint16_t i = 0; i < count; i++) {
                local_pulses[i] = pulses[i];
            }

            int8_t rssi_raw = (int8_t)cc1101_read_status(CC1101_RSSI);
            float rssi_dbm = (rssi_raw / 2.0f) - 74.0f;

            printf("\n\033[1;32m=== PACCHETTO CATTURATO (%u fronti | RSSI: %.1f dBm) ===\033[0m\n", count, rssi_dbm);
            decode_ook_pwm(local_pulses, count);

            printf("RAW (us): ");
            for (uint16_t i = 0; i < count; i++) {
                printf("%+" PRId32 " ", local_pulses[i]);
                if ((i + 1) % 8 == 0) printf("\n          "); // A capo ogni 8 valori per terminale stretto
            }
            printf("\n====================================================\n");

            // Ri-arma la cattura
            pulse_count = 0;
            packet_ready = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}