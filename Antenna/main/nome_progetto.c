#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cc1101.h"

static const char *TAG = "RF_DECK";

// Pin Pulsantiera Tattile (attivi bassi con Pull-Up interno)
#define BTN_FREQ_PIN    4
#define BTN_REC_PIN     5
#define BTN_SLOT_PIN    6
#define BTN_TX_PIN      7

#define NUM_SLOTS       5
#define DEBOUNCE_US     40000    // 40 ms
#define LONG_PRESS_US   900000   // 0.9 s

// Bande preset
static const struct {
    uint32_t freq_hz;
    const char *label;
} FREQ_PRESETS[] = {
    { 433920000, "433.92 MHz (EU ISM Standard)" },
    { 433420000, "433.42 MHz (Tapparelle/Domotica)" },
    { 868350000, "868.35 MHz (Allarmi/Sensori EU)" },
    { 315000000, "315.00 MHz (Import/US)" },
};
#define NUM_PRESETS (sizeof(FREQ_PRESETS) / sizeof(FREQ_PRESETS[0]))

typedef struct {
    gpio_num_t pin;
    bool last_raw;
    bool stable_state;
    int64_t last_debounce_time;
    int64_t press_start_time;
    bool long_handled;
} button_t;

static rf_slot_t slots[NUM_SLOTS];
static uint8_t active_slot = 0;
static uint8_t preset_idx = 0;

static void print_status_bar(void) {
    printf("\n\033[1;36m[STATO] Banda: %.2f MHz | Slot: %u/%u | RX: %s\033[0m\n",
           cc1101_get_frequency() / 1e6,
           active_slot,
           NUM_SLOTS - 1,
           cc1101_is_rx_armed() ? "\033[1;32mARMED\033[0m" : "\033[1;33mLOCKED\033[0m");
}

static void print_slot_info(uint8_t idx) {
    rf_slot_t *s = &slots[idx];
    if (!s->valid) {
        printf("[SLOT %u] Vuoto.\n", idx);
        return;
    }
    printf("[SLOT %u] Freq: %.2f MHz | Fronti: %u | RSSI: %.1f dBm",
           idx, s->freq_hz / 1e6, s->pulse_count, s->rssi_dbm);
    if (s->bit_len > 0) {
        printf(" | Bit: %u | HEX: 0x%llX", s->bit_len, s->hex_code);
    }
    printf("\n");
}

// Genera un pacchetto OOK sintetico stile EV1527 (24 bit) per testare TX senza telecomandi
static void synthesize_test_packet(rf_slot_t *s, uint32_t code24) {
    uint16_t idx = 0;
    for (int b = 23; b >= 0; b--) {
        bool bit = (code24 >> b) & 1;
        if (bit) {
            s->pulses[idx++] = 960;   // Impulso lungo = 1
            s->pulses[idx++] = -320;
        } else {
            s->pulses[idx++] = 320;   // Impulso corto = 0
            s->pulses[idx++] = -960;
        }
    }
    s->pulses[idx - 1] = -9800; // Sync gap finale
    s->valid = true;
    s->freq_hz = cc1101_get_frequency();
    s->rssi_dbm = 0.0f;
    s->pulse_count = idx;
    s->bit_len = 24;
    s->hex_code = code24 & 0xFFFFFF;
}

static void handle_buttons(button_t *btns, size_t count) {
    int64_t now = esp_timer_get_time();

    for (size_t i = 0; i < count; i++) {
        bool raw = gpio_get_level(btns[i].pin); // true = rilasciato, false = premuto

        if (raw != btns[i].last_raw) {
            btns[i].last_debounce_time = now;
            btns[i].last_raw = raw;
        }

        if ((now - btns[i].last_debounce_time) > DEBOUNCE_US) {
            if (raw != btns[i].stable_state) {
                btns[i].stable_state = raw;

                if (!raw) {
                    // Fronte di pressione
                    btns[i].press_start_time = now;
                    btns[i].long_handled = false;
                } else {
                    // Fronte di rilascio -> Click Breve (se non già gestito come lungo)
                    if (!btns[i].long_handled) {
                        switch (btns[i].pin) {
                            case BTN_FREQ_PIN:
                                preset_idx = (preset_idx + 1) % NUM_PRESETS;
                                cc1101_set_frequency(FREQ_PRESETS[preset_idx].freq_hz);
                                printf("\n>> Banda cambiata: %s\n", FREQ_PRESETS[preset_idx].label);
                                print_status_bar();
                                break;

                            case BTN_REC_PIN:
                                cc1101_set_rx_armed(!cc1101_is_rx_armed());
                                print_status_bar();
                                break;

                            case BTN_SLOT_PIN:
                                active_slot = (active_slot + 1) % NUM_SLOTS;
                                print_status_bar();
                                print_slot_info(active_slot);
                                break;

                            case BTN_TX_PIN:
                                if (slots[active_slot].valid) {
                                    // Allinea frequenza del chip a quella registrata nello slot
                                    if (cc1101_get_frequency() != slots[active_slot].freq_hz) {
                                        cc1101_set_frequency(slots[active_slot].freq_hz);
                                    }
                                    printf("\033[1;31m[TX] Trasmissione Slot %u (3x) su %.2f MHz...\033[0m\n",
                                           active_slot, slots[active_slot].freq_hz / 1e6);
                                    cc1101_transmit_raw(slots[active_slot].pulses,
                                                        slots[active_slot].pulse_count, 3);
                                } else {
                                    printf("[TX] Slot %u vuoto, nulla da trasmettere.\n", active_slot);
                                }
                                break;
                            default:
                                break;
                        }
                    }
                }
            } else if (!btns[i].stable_state) {
                // Tasto mantenuto premuto
                int64_t held_us = now - btns[i].press_start_time;

                if (btns[i].pin == BTN_TX_PIN && held_us > 500000 && slots[active_slot].valid) {
                    // Raffica continua finché il tasto TX resta premuto
                    if (cc1101_get_frequency() != slots[active_slot].freq_hz) {
                        cc1101_set_frequency(slots[active_slot].freq_hz);
                    }
                    printf("\033[1;31m[TX BURST] Slot %u...\033[0m\n", active_slot);
                    cc1101_transmit_raw(slots[active_slot].pulses, slots[active_slot].pulse_count, 1);
                    btns[i].long_handled = true;
                } else if (!btns[i].long_handled && held_us > LONG_PRESS_US) {
                    btns[i].long_handled = true;
                    if (btns[i].pin == BTN_FREQ_PIN) {
                        preset_idx = 0;
                        cc1101_set_frequency(FREQ_PRESETS[0].freq_hz);
                        printf("\n>> Reset banda a: %s\n", FREQ_PRESETS[0].label);
                        print_status_bar();
                    } else if (btns[i].pin == BTN_REC_PIN) {
                        memset(slots, 0, sizeof(slots));
                        active_slot = 0;
                        printf("\n>> Memoria cancellata: tutti gli slot svuotati.\n");
                        print_status_bar();
                    }
                }
            }
        }
    }
}

// CLI non bloccante su terminale (comoda per testare senza telecomandi)
static void handle_serial_cli(void) {
    static char cmd_buf[64];
    static uint8_t cmd_len = 0;
    int c = getchar();

    if (c == EOF) return;

    if (c == '\n' || c == '\r') {
        if (cmd_len == 0) return;
        cmd_buf[cmd_len] = '\0';
        cmd_len = 0;

        if (strcmp(cmd_buf, "list") == 0) {
            printf("\n--- MEMORIA SLOT ---\n");
            for (uint8_t i = 0; i < NUM_SLOTS; i++) {
                printf("%c ", (i == active_slot) ? '>' : ' ');
                print_slot_info(i);
            }
        } else if (strncmp(cmd_buf, "gen ", 4) == 0) {
            uint32_t val = (uint32_t)strtoul(cmd_buf + 4, NULL, 16);
            synthesize_test_packet(&slots[active_slot], val);
            printf("\n[GEN] Sintetizzato pacchetto test 0x%06" PRIX32 " nello Slot %u\n", val, active_slot);
            print_slot_info(active_slot);
        } else if (strcmp(cmd_buf, "tx") == 0) {
            if (slots[active_slot].valid) {
                printf("\033[1;31m[TX] Trasmissione Slot %u (3x)...\033[0m\n", active_slot);
                cc1101_transmit_raw(slots[active_slot].pulses, slots[active_slot].pulse_count, 3);
            } else {
                printf("[TX] Slot %u vuoto.\n", active_slot);
            }
        } else {
            printf("Comandi: list | gen <hex24> (es. gen A5F012) | tx\n");
        }
    } else if (cmd_len < sizeof(cmd_buf) - 1) {
        cmd_buf[cmd_len++] = (char)c;
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(cc1101_init());

    uint8_t ver = cc1101_get_version();
    uint8_t state = cc1101_get_marcstate();
    ESP_LOGI(TAG, "RF Deck avviato | CC1101 VER: 0x%02X | STATE: 0x%02X", ver, state);

    // Configura i 4 GPIO dei tasti con Pull-Up interno
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_FREQ_PIN) | (1ULL << BTN_REC_PIN) |
                        (1ULL << BTN_SLOT_PIN) | (1ULL << BTN_TX_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    button_t buttons[] = {
        { BTN_FREQ_PIN, true, true, 0, 0, false },
        { BTN_REC_PIN,  true, true, 0, 0, false },
        { BTN_SLOT_PIN, true, true, 0, 0, false },
        { BTN_TX_PIN,   true, true, 0, 0, false },
    };

    print_status_bar();
    printf("Comandi seriali opzionali: list | gen A5F012 | tx\n");

    while (1) {
        // 1. Controlla catture radio dall'interrupt
        if (cc1101_fetch_packet(&slots[active_slot])) {
            printf("\n\033[1;32m=== CATTURA SALVATA IN SLOT %u ===\033[0m\n", active_slot);
            print_slot_info(active_slot);
            printf("RAW (us): ");
            for (uint16_t i = 0; i < slots[active_slot].pulse_count; i++) {
                printf("%+" PRId32 " ", slots[active_slot].pulses[i]);
                if ((i + 1) % 8 == 0) printf("\n          ");
            }
            printf("\n==================================\n");
        }

        // 2. Gestione 4 tasti fisici
        handle_buttons(buttons, 4);

        // 3. Gestione comandi da terminale
        handle_serial_cli();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}