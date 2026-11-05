#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED_PIN GPIO_NUM_4

void app_main(void)
{
    // Inizializza il pin GPIO 4 come uscita digitale
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    while (1) {
        printf("LED ACCESO GODO\n");
        gpio_set_level(LED_PIN, 1); // Invia 3.3V al LED
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Aspetta 1 secondo

    }
