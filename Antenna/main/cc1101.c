#include "cc1101.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "CC1101_DRV";

// Command Strobes
#define CC1101_SRES         0x30
#define CC1101_SRX          0x34
#define CC1101_STX          0x35
#define CC1101_SIDLE        0x36

// Registri di Configurazione
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
#define CC1101_MCSM0        0x18
#define CC1101_AGCCTRL2     0x1B
#define CC1101_AGCCTRL1     0x1C
#define CC1101_AGCCTRL0     0x1D
#define CC1101_FREND0       0x22
#define CC1101_FSCAL3       0x23
#define CC1101_FSCAL2       0x24
#define CC1101_FSCAL1       0x25
#define CC1101_FSCAL0       0x26

// Registri di Stato e Tabelle
#define CC1101_VERSION      0x31
#define CC1101_RSSI         0x34
#define CC1101_MARCSTATE    0x35
#define CC1101_PATABLE      0x3E
#define CC1101_WRITE_BURST  0x40
#define CC1101_READ_BURST   0xC0

static spi_device_handle_t spi_handle;
static uint32_t current_freq_hz = 433920000;

// Stato condiviso con l'interrupt hardware GDO0
static volatile int32_t isr_pulses[CC1101_MAX_PULSES];
static volatile uint16_t isr_pulse_count = 0;
static volatile int64_t isr_last_edge = 0;
static volatile bool isr_packet_ready = false;
static volatile bool rx_armed = true;

static void cc1101_strobe(uint8_t strobe) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &strobe,
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi_handle, &t));
}

static void cc1101_write_reg(uint8_t addr, uint8_t value) {
    uint8_t tx_buf[2] = { addr, value };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi_handle, &t));
}

static void cc1101_write_patable_ook(void) {
    // Indice 0 = 0x00 (portante spenta per bit 0), Indice 1 = 0xC0 (+10 dBm per bit 1)
    uint8_t tx_buf[3] = { (uint8_t)(CC1101_PATABLE | CC1101_WRITE_BURST), 0x00, 0xC0 };
    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx_buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi_handle, &t));
}

static uint8_t cc1101_read_status(uint8_t addr) {
    uint8_t tx_buf[2] = { (uint8_t)(addr | CC1101_READ_BURST), 0x00 };
    uint8_t rx_buf[2] = { 0, 0 };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi_handle, &t));
    return rx_buf[1];
}

static void IRAM_ATTR gdo0_isr_handler(void *arg) {
    if (!rx_armed || isr_packet_ready) {
        return;
    }

    int64_t now = esp_timer_get_time();
    int64_t dt = now - isr_last_edge;
    isr_last_edge = now;

    int current_level = gpio_get_level(CC1101_PIN_GDO0);
    int ended_level = !current_level;

    if (dt > CC1101_GAP_US) {
        if (isr_pulse_count >= CC1101_MIN_PULSES) {
            if (isr_pulse_count < CC1101_MAX_PULSES) {
                isr_pulses[isr_pulse_count++] = ended_level ? (int32_t)dt : -(int32_t)dt;
            }
            isr_packet_ready = true;
        } else {
            isr_pulse_count = 0;
        }
        return;
    }

    if (dt < CC1101_NOISE_MIN_US) {
        isr_pulse_count = 0;
        return;
    }

    if (isr_pulse_count < CC1101_MAX_PULSES) {
        isr_pulses[isr_pulse_count++] = ended_level ? (int32_t)dt : -(int32_t)dt;
    } else {
        isr_packet_ready = true;
    }
}

static void decode_pwm_to_slot(rf_slot_t *slot) {
    int32_t min_high = INT32_MAX;
    int32_t max_high = 0;

    for (uint16_t i = 0; i < slot->pulse_count; i++) {
        int32_t p = slot->pulses[i];
        if (p > 0 && p < CC1101_GAP_US) {
            if (p < min_high) min_high = p;
            if (p > max_high) max_high = p;
        }
    }

    slot->bit_len = 0;
    slot->hex_code = 0;

    if (min_high == INT32_MAX || max_high < (min_high * 1.6)) {
        return;
    }

    int32_t threshold = (min_high + max_high) / 2;
    for (uint16_t i = 0; i < slot->pulse_count; i++) {
        int32_t p = slot->pulses[i];
        if (p > 0 && p < CC1101_GAP_US) {
            uint64_t bit = (p > threshold) ? 1ULL : 0ULL;
            if (slot->bit_len < 64) {
                slot->hex_code = (slot->hex_code << 1) | bit;
            }
            slot->bit_len++;
        }
    }
}

esp_err_t cc1101_init(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = CC1101_PIN_MISO,
        .mosi_io_num = CC1101_PIN_MOSI,
        .sclk_io_num = CC1101_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CC1101_PIN_CSN,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle));

    cc1101_strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Configurazione base asincrona ASK/OOK
    cc1101_write_reg(CC1101_IOCFG0,   0x0D); // GDO0: Serial Data Output in RX
    cc1101_write_reg(CC1101_PKTCTRL0, 0x32); // Modalità seriale asincrona
    cc1101_write_reg(CC1101_FSCTRL1,  0x06);

    cc1101_write_reg(CC1101_MDMCFG4,  0x87); // Banda filtro ~203 kHz
    cc1101_write_reg(CC1101_MDMCFG3,  0x83);
    cc1101_write_reg(CC1101_MDMCFG2,  0x30); // ASK/OOK, no sync-word
    cc1101_write_reg(CC1101_MDMCFG1,  0x22);
    cc1101_write_reg(CC1101_MDMCFG0,  0xF8);

    // Auto-calibrazione PLL ad ogni passaggio IDLE -> RX o IDLE -> TX
    cc1101_write_reg(CC1101_MCSM0,    0x14);

    // AGC ottimizzato per treni d'impulsi OOK
    cc1101_write_reg(CC1101_AGCCTRL2, 0x03);
    cc1101_write_reg(CC1101_AGCCTRL1, 0x00);
    cc1101_write_reg(CC1101_AGCCTRL0, 0x91);

    // FREND0 = 0x11 seleziona PATABLE[1] per l'1 logico in OOK TX
    cc1101_write_reg(CC1101_FREND0,   0x11);
    cc1101_write_patable_ook();

    // Calibrazione sintetizzatore
    cc1101_write_reg(CC1101_FSCAL3,   0xE9);
    cc1101_write_reg(CC1101_FSCAL2,   0x2A);
    cc1101_write_reg(CC1101_FSCAL1,   0x00);
    cc1101_write_reg(CC1101_FSCAL0,   0x1F);

    // Configura GDO0 come ingresso interrupt
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CC1101_PIN_GDO0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CC1101_PIN_GDO0, gdo0_isr_handler, NULL));

    cc1101_set_frequency(433920000);
    return ESP_OK;
}

uint8_t cc1101_get_version(void) {
    return cc1101_read_status(CC1101_VERSION);
}

uint8_t cc1101_get_marcstate(void) {
    return cc1101_read_status(CC1101_MARCSTATE) & 0x1F;
}

float cc1101_read_rssi_dbm(void) {
    int8_t raw = (int8_t)cc1101_read_status(CC1101_RSSI);
    return (raw / 2.0f) - 74.0f;
}

void cc1101_set_frequency(uint32_t freq_hz) {
    gpio_intr_disable(CC1101_PIN_GDO0);
    cc1101_strobe(CC1101_SIDLE);

    // Calcolo parola a 24 bit per quarzo a 26 MHz: (freq_hz * 2^16) / 26_000_000
    uint64_t freq_reg = ((uint64_t)freq_hz << 16) / 26000000ULL;
    cc1101_write_reg(CC1101_FREQ2, (uint8_t)((freq_reg >> 16) & 0xFF));
    cc1101_write_reg(CC1101_FREQ1, (uint8_t)((freq_reg >> 8) & 0xFF));
    cc1101_write_reg(CC1101_FREQ0, (uint8_t)(freq_reg & 0xFF));

    current_freq_hz = freq_hz;
    isr_pulse_count = 0;
    isr_packet_ready = false;

    cc1101_strobe(CC1101_SRX);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_intr_enable(CC1101_PIN_GDO0);
}

uint32_t cc1101_get_frequency(void) {
    return current_freq_hz;
}

void cc1101_set_rx_armed(bool armed) {
    rx_armed = armed;
    if (armed) {
        isr_pulse_count = 0;
        isr_packet_ready = false;
    }
}

bool cc1101_is_rx_armed(void) {
    return rx_armed;
}

bool cc1101_fetch_packet(rf_slot_t *out_slot) {
    if (!rx_armed) return false;

    // Flush se un pacchetto singolo è seguito da silenzio > 10 ms
    if (!isr_packet_ready && isr_pulse_count >= CC1101_MIN_PULSES) {
        if ((esp_timer_get_time() - isr_last_edge) > 10000) {
            isr_packet_ready = true;
        }
    }

    if (!isr_packet_ready) return false;

    uint16_t count = isr_pulse_count;
    for (uint16_t i = 0; i < count; i++) {
        out_slot->pulses[i] = isr_pulses[i];
    }

    out_slot->valid = true;
    out_slot->freq_hz = current_freq_hz;
    out_slot->rssi_dbm = cc1101_read_rssi_dbm();
    out_slot->pulse_count = count;
    decode_pwm_to_slot(out_slot);

    isr_pulse_count = 0;
    isr_packet_ready = false;
    return true;
}

void cc1101_transmit_raw(const int32_t *pulses, uint16_t count, uint8_t repeats) {
    if (count == 0 || repeats == 0) return;

    // 1. Disabilita interrupt RX e metti il CC1101 in IDLE
    gpio_intr_disable(CC1101_PIN_GDO0);
    cc1101_strobe(CC1101_SIDLE);

    // 2. Metti GDO0 lato CC1101 in High-Z (0x2E) così l'ESP32 può pilotarlo come uscita
    cc1101_write_reg(CC1101_IOCFG0, 0x2E);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_OUTPUT);
    gpio_set_level(CC1101_PIN_GDO0, 0);

    // 3. Entra in TX e attendi ~1.2 ms per la calibrazione automatica del PLL
    cc1101_strobe(CC1101_STX);
    esp_rom_delay_us(1200);

    // 4. Riproduci gli impulsi al microsecondo
    for (uint8_t r = 0; r < repeats; r++) {
        for (uint16_t i = 0; i < count; i++) {
            int32_t p = pulses[i];
            if (p > 0) {
                gpio_set_level(CC1101_PIN_GDO0, 1);
                esp_rom_delay_us((uint32_t)p);
            } else {
                gpio_set_level(CC1101_PIN_GDO0, 0);
                esp_rom_delay_us((uint32_t)(-p));
            }
        }
        gpio_set_level(CC1101_PIN_GDO0, 0);
        esp_rom_delay_us(6000); // Pausa inter-frame
    }

    // 5. Torna in IDLE e ripristina GDO0 come ingresso RX
    cc1101_strobe(CC1101_SIDLE);
    gpio_set_direction(CC1101_PIN_GDO0, GPIO_MODE_INPUT);
    cc1101_write_reg(CC1101_IOCFG0, 0x0D);

    isr_pulse_count = 0;
    isr_packet_ready = false;
    cc1101_strobe(CC1101_SRX);
    gpio_intr_enable(CC1101_PIN_GDO0);
}