#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" //task e thread
#include "driver/gpio.h" //definisce nomi pin

// Array dei pin GPIO collegati ai 4 LED
//li metti nella flash costante, non cambiano mai
static const gpio_num_t leds[] = {
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7
};

// Calcolo dinamico della dimensione dell'array prima di compilare
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

void app_main(void)
{
    // 1. Inizializzazione dei pin come uscite digitali
    for (int i = 0; i < NUM_LEDS; i++) {
        gpio_reset_pin(leds[i]);//resetta al default , prassi
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);//segna come uscita cosi gli puo passa corrente
    }

    int indice = 0;//qual'è attivo
    int direzione = 1; // 1 = avanza (da GPIO 4 a 7), -1 = torna indietro (da GPIO 7 a 4)

    // 2. Ciclo infinito principale
    while (1) {
        // Spegne tutti i LED per sicurezza ad ogni passo
        for (int j = 0; j < NUM_LEDS; j++) {
            gpio_set_level(leds[j], 0);//spegni tutto prima di inziiare
        }

        // Accende solo il LED corrente nella posizione 'indice'
        gpio_set_level(leds[indice], 1);
        printf("LED attivo: GPIO %d (Indice: %d)\n", leds[indice], indice);

        // Calcola la posizione per il passo successivo
        indice += direzione;

        // Inverte il senso di marcia quando si raggiungono i capi dell'array
        if (indice >= (int)NUM_LEDS - 1) {
            direzione = -1; // Arrivato a GPIO 7, torna indietro verso GPIO 4
        } else if (indice <= 0) {
            direzione = 1;  // Arrivato a GPIO 4, riparte in avanti verso GPIO 7
        }

        // Mantiene lo stato attuale per 150 millisecondi
        vTaskDelay(pdMS_TO_TICKS(1000));//
    }
}