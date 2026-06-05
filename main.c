#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#define DATA_LEN   64
#define TARGET     2000
#define DROP_PROB  0.2f
#define TIMEOUT_MS 200

typedef struct {
    uint16_t seqNum;
    uint8_t  payload[DATA_LEN];
    uint16_t checksum;
} Packet_t;

typedef struct {
    uint32_t sent;
    uint32_t received;
    uint32_t dropped;
    uint32_t retransmitted;
    uint32_t timeouts;
} Stats_t;

static Stats_t stats = {0};
static volatile uint8_t simulationDone = 0;

static QueueHandle_t     xTxQueue;
static QueueHandle_t     xRxQueue;
static QueueHandle_t     xAckQueue;
static SemaphoreHandle_t xAckSem;
static SemaphoreHandle_t xStatsMutex;

/* ===== PLACEHOLDER TASK (remove on Day 6) ===== */
void vHelloTask(void *pvParameters) {
    for (;;) {
        printf("Phase 1 skeleton running\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

int main(void) {
    /* Create IPC objects */
    xTxQueue    = xQueueCreate(10, sizeof(Packet_t));
    xRxQueue    = xQueueCreate(10, sizeof(Packet_t));
    xAckQueue   = xQueueCreate(10, sizeof(uint16_t));
    xAckSem     = xSemaphoreCreateBinary();
    xStatsMutex = xSemaphoreCreateMutex();

    configASSERT(xTxQueue);
    configASSERT(xRxQueue);
    configASSERT(xAckQueue);
    configASSERT(xAckSem);
    configASSERT(xStatsMutex);

    xTaskCreate(vHelloTask, "Hello", 256, NULL, 1, NULL);

    vTaskStartScheduler();
    return 0;
}