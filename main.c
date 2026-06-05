#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

void vHelloTask(void *pvParameters) {
    for (;;) {
        printf("Task running\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

int main(void) {
    xTaskCreate(vHelloTask, "Hello", 256, NULL, 1, NULL);
    vTaskStartScheduler();
    return 0;
}