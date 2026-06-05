#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#define DATA_LEN 64
#define TARGET 2000

#ifndef DROP_PROB
#define DROP_PROB 0.2f
#endif

#ifndef TIMEOUT_MS
#define TIMEOUT_MS 200
#endif

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

static QueueHandle_t xGenQueue;
static QueueHandle_t xTxQueue;
static QueueHandle_t xRxQueue;
static QueueHandle_t xAckQueue;
static SemaphoreHandle_t xAckSem;
static SemaphoreHandle_t xStatsMutex;

static Packet_t currentPkt;

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

        xQueueSend(xGenQueue, &pkt, portMAX_DELAY);
        seqNum++;
    }
}

void vSenderTask(void *pvParameters)
{
    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        xQueueReceive(xGenQueue, &currentPkt, portMAX_DELAY);

        xSemaphoreTake(xStatsMutex, portMAX_DELAY);
        stats.sent++;
        xSemaphoreGive(xStatsMutex);

        xQueueSend(xTxQueue, &currentPkt, portMAX_DELAY);

        while (xSemaphoreTake(xAckSem, pdMS_TO_TICKS(TIMEOUT_MS)) == pdFALSE)
        {
            stats.retransmitted++;
            stats.timeouts++;
            xQueueSend(xTxQueue, &currentPkt, portMAX_DELAY);
        }
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
            stats.dropped++;
        }
        else
        {
            xQueueSend(xRxQueue, &pkt, portMAX_DELAY);
        }
    }
}

/* ===== ENGINEER 2 — RX ===== */

void vReceiverTask(void *pvParameters)
{
    Packet_t pkt;
    uint16_t expectedSeq = 0;

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        xQueueReceive(xRxQueue, &pkt, portMAX_DELAY);

        if (pkt.seqNum == expectedSeq)
        {
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.received++;
            uint32_t rcvd = stats.received;
            xSemaphoreGive(xStatsMutex);

            xQueueSend(xAckQueue, &pkt.seqNum, 0);
            expectedSeq++;

            if (rcvd >= TARGET)
            {
                simulationDone = 1;
            }
        }
        /* duplicates silently discarded */
    }
}

void vAckRelayTask(void *pvParameters)
{
    uint16_t ackSeq;

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        xQueueReceive(xAckQueue, &ackSeq, portMAX_DELAY);
        xSemaphoreGive(xAckSem);
    }
}

void vStatsPrinterTask(void *pvParameters)
{
    while (!simulationDone)
        vTaskDelay(pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(200));

    xSemaphoreTake(xStatsMutex, portMAX_DELAY);
    printf("\n===== SIMULATION COMPLETE =====\n");
    printf("STATS: %u,%u,%u,%u,%u\n",
           stats.sent, stats.received,
           stats.dropped, stats.retransmitted, stats.timeouts);
    printf("Sent:          %u\n", stats.sent);
    printf("Received:      %u\n", stats.received);
    printf("Dropped:       %u\n", stats.dropped);
    printf("Retransmitted: %u\n", stats.retransmitted);
    printf("Timeouts:      %u\n", stats.timeouts);
    printf("Drop rate:     %.1f%%\n",
           100.0f * stats.dropped / (stats.dropped + stats.received));
    xSemaphoreGive(xStatsMutex);

    exit(0);
}

int main(void)
{
    xGenQueue = xQueueCreate(10, sizeof(Packet_t));
    xTxQueue = xQueueCreate(10, sizeof(Packet_t));
    xRxQueue = xQueueCreate(10, sizeof(Packet_t));
    xAckQueue = xQueueCreate(10, sizeof(uint16_t));
    xAckSem = xSemaphoreCreateBinary();
    xStatsMutex = xSemaphoreCreateMutex();

    configASSERT(xGenQueue);
    configASSERT(xTxQueue);
    configASSERT(xRxQueue);
    configASSERT(xAckQueue);
    configASSERT(xAckSem);
    configASSERT(xStatsMutex);

    xTaskCreate(vPacketGeneratorTask, "Gen", 512, NULL, 1, NULL);
    xTaskCreate(vSenderTask, "Sender", 512, NULL, 3, NULL);
    xTaskCreate(vLinkSimTask, "Link", 512, NULL, 2, NULL);
    xTaskCreate(vReceiverTask, "RX", 512, NULL, 2, NULL);
    xTaskCreate(vAckRelayTask, "ACK", 512, NULL, 3, NULL);
    xTaskCreate(vStatsPrinterTask, "Stats", 512, NULL, 1, NULL);

    vTaskStartScheduler();
    return 0;
}