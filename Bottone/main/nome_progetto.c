#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED_PIN      GPIO_NUM_4
#define BUTTON_PIN   GPIO_NUM_9

void app_main(void)
{
    // 1. Configura il LED come Uscita
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // 2. Configura il Pulsante come Ingresso con Pull-Up INTERNO
    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);//da input
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);//non farlo fluttuare in aria

    printf("Sistema pronto! Premi il pulsante su GPIO 9 per accendere il LED.\n");

    while (1) {
        // Legge lo stato del pin (0 = premuto/GND, 1 = rilasciato/Pull-Up)
        int stato_pulsante = gpio_get_level(BUTTON_PIN);

        if (stato_pulsante == 0) {
            // Pulsante PREMUTO -> Accendi LED
            gpio_set_level(LED_PIN, 1);
            printf("Pulsante PREMUTO! LED Acceso.\n");
        } else {
            // Pulsante RILASCIATO -> Spegni LED
            gpio_set_level(LED_PIN, 0);
        }

        // Pausa di 50 ms per non saturare la CPU e cedere controllo a FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}