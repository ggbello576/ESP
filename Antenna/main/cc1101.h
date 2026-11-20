#ifndef CC1101_H
#define CC1101_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Pinout ESP32-S3 -> CC1101
#define CC1101_PIN_MISO     13
#define CC1101_PIN_MOSI     11
#define CC1101_PIN_SCK      12
#define CC1101_PIN_CSN      10
#define CC1101_PIN_GDO0     15
#define CC1101_PIN_GDO2     16

// Parametri cattura impulsi RAW
#define CC1101_MAX_PULSES   256
#define CC1101_MIN_PULSES   24
#define CC1101_NOISE_MIN_US 120
#define CC1101_GAP_US       4000

typedef struct {
    bool valid;
    uint32_t freq_hz;
    float rssi_dbm;
    uint16_t pulse_count;
    int32_t pulses[CC1101_MAX_PULSES];
    uint16_t bit_len;
    uint64_t hex_code;
} rf_slot_t;

esp_err_t cc1101_init(void);
uint8_t cc1101_get_version(void);
uint8_t cc1101_get_marcstate(void);
float cc1101_read_rssi_dbm(void);

void cc1101_set_frequency(uint32_t freq_hz);
uint32_t cc1101_get_frequency(void);

void cc1101_set_rx_armed(bool armed);
bool cc1101_is_rx_armed(void);

// Controlla se l'ISR ha catturato un nuovo pacchetto e lo copia in out_slot
bool cc1101_fetch_packet(rf_slot_t *out_slot);

// Trasmette un pacchetto RAW in OOK alla massima potenza (+10 dBm)
void cc1101_transmit_raw(const int32_t *pulses, uint16_t count, uint8_t repeats);

#endif // CC1101_H