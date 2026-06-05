#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <timers.h>

#define DATA_LEN 64
#define TARGET 2000
#define DROP_PROB 0.2f
#define TIMEOUT_MS 200

typedef struct
{
    uint16_t seqNum;
    uint8_t payload[DATA_LEN];
    uint16_t checksum;
} Packet_t;

typedef struct
{
    uint32_t sent;
    uint32_t received;
    uint32_t dropped;
    uint32_t retransmitted;
    uint32_t timeouts;
} Stats_t;

static Stats_t stats = {0};
static volatile uint8_t simulationDone = 0;

static QueueHandle_t xTxQueue;
static QueueHandle_t xRxQueue;
static QueueHandle_t xAckQueue;
static SemaphoreHandle_t xAckSem;
static SemaphoreHandle_t xStatsMutex;

/* ===== ENGINEER 1 — TX ===== */

void vPacketGeneratorTask(void *pvParameters)
{
    uint16_t seqNum = 0;
    Packet_t pkt;

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        pkt.seqNum = seqNum;
        pkt.checksum = seqNum;
        memset(pkt.payload, seqNum & 0xFF, DATA_LEN);

        xQueueSend(xTxQueue, &pkt, portMAX_DELAY);
        seqNum++;
    }
}
void vLinkSimTask(void *pvParameters)
{
    Packet_t pkt;

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        xQueueReceive(xTxQueue, &pkt, portMAX_DELAY);

        float r = (float)rand() / (float)RAND_MAX;
        if (r < DROP_PROB)
        {
            /* Packet dropped — update stats */
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.dropped++;
            xSemaphoreGive(xStatsMutex);
        }
        else
        {
            /* Packet survives — forward to receiver */
            xQueueSend(xRxQueue, &pkt, portMAX_DELAY);
        }
    }
}
/* Priority: 2 */
/* ===== SENDER FSM ===== */

static TimerHandle_t xRetxTimer;
static Packet_t currentPkt; /* packet in flight */

void vRetxTimerCallback(TimerHandle_t xTimer)
{
    /* Timeout fired — retransmit */
    xSemaphoreTake(xStatsMutex, portMAX_DELAY);
    stats.retransmitted++;
    stats.timeouts++;
    xSemaphoreGive(xStatsMutex);

    xQueueSend(xTxQueue, &currentPkt, 0); /* re-inject into link */
    xTimerStart(xRetxTimer, 0);           /* restart timer */
}

void vSenderTask(void *pvParameters)
{
    xRetxTimer = xTimerCreate(
        "RetxTimer",
        pdMS_TO_TICKS(TIMEOUT_MS),
        pdFALSE, /* one-shot */
        NULL,
        vRetxTimerCallback);
    configASSERT(xRetxTimer);

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        /* Grab next packet from generator */
        xQueueReceive(xTxQueue, &currentPkt, portMAX_DELAY);

        xSemaphoreTake(xStatsMutex, portMAX_DELAY);
        stats.sent++;
        xSemaphoreGive(xStatsMutex);

        /* Forward to link sim and start timeout */
        xQueueSend(xTxQueue, &currentPkt, portMAX_DELAY);
        xTimerStart(xRetxTimer, 0);

        /* Block until ACK relay gives the semaphore */
        xSemaphoreTake(xAckSem, portMAX_DELAY);
        xTimerStop(xRetxTimer, 0);
    }
}
/* Priority: 3 */
/* Priority: 1 */
int main(void)
{

    xTxQueue = xQueueCreate(10, sizeof(Packet_t));
    xRxQueue = xQueueCreate(10, sizeof(Packet_t));
    xAckQueue = xQueueCreate(10, sizeof(uint16_t));
    xAckSem = xSemaphoreCreateBinary();
    xStatsMutex = xSemaphoreCreateMutex();

    configASSERT(xTxQueue);
    configASSERT(xRxQueue);
    configASSERT(xAckQueue);
    configASSERT(xAckSem);
    configASSERT(xStatsMutex);

    xTaskCreate(vPacketGeneratorTask, "Gen", 512, NULL, 1, NULL);
    xTaskCreate(vSenderTask, "Sender", 512, NULL, 3, NULL);
    xTaskCreate(vLinkSimTask, "Link", 512, NULL, 2, NULL);
    vTaskStartScheduler();
    return 0;
}