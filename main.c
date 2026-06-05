#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ===== PARAMETERS ===== */
#ifndef TARGET
#define TARGET    2000
#endif

#define L1        500
#define L2        1500
#define T1_MS     100
#define T2_MS     200
#define K         40
#define C_BITS    100000
#define P_ACK     0.01f
#define D_MS      5
#define MAX_RETX  4

#ifndef DROP_PROB
#define DROP_PROB  0.01f
#endif

#ifndef TIMEOUT_MS
#define TIMEOUT_MS 200
#endif
/* ===== PACKET STRUCTURES ===== */
typedef struct
{
    uint8_t senderId;
    uint8_t destId;
    uint16_t length;
    uint32_t seqNum;
} Header_t;

typedef struct
{
    Header_t header;
    uint8_t payload[L2]; /* max possible payload */
} Packet_t;

typedef struct
{
    uint8_t srcNode;
    uint8_t destNode;
    uint32_t seqNum;
} Ack_t;

/* ===== STATS ===== */
typedef struct
{
    uint32_t sent;
    uint32_t received;
    uint32_t droppedByLink;
    uint32_t retransmitted;
    uint32_t timeouts;
    uint32_t droppedMaxRetx;
    uint64_t bytesReceived;
} Stats_t;

static Stats_t stats = {0};
static volatile uint8_t simulationDone = 0;
static struct timespec tStart, tEnd;

/* ===== IPC HANDLES ===== */
static QueueHandle_t xGenQueue;
static QueueHandle_t xTxQueue;
static QueueHandle_t xRxQueue;
static QueueHandle_t xAckTxQueue; /* Receiver → ACK link sim */
static QueueHandle_t xAckRxQueue; /* ACK link sim → Sender */
static SemaphoreHandle_t xAckSem;
static SemaphoreHandle_t xStatsMutex;

/* ===== HELPERS ===== */
static uint16_t randRange(uint16_t lo, uint16_t hi)
{
    return lo + (uint16_t)(rand() % (hi - lo + 1));
}

static float randFloat(void)
{
    return (float)rand() / (float)RAND_MAX;
}

/* transmission delay in ms for a packet of length bytes */
static uint32_t txDelayMs(uint16_t length)
{
    return (uint32_t)(((uint32_t)length * 8 * 1000) / C_BITS);
}

/* ===== ENGINEER 1 — TX ===== */

void vPacketGeneratorTask(void *pvParameters)
{
    uint32_t seqNum = 0;

    clock_gettime(CLOCK_MONOTONIC, &tStart);

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        Packet_t *pkt = (Packet_t *)malloc(sizeof(Packet_t));
        configASSERT(pkt);

        uint16_t L = randRange(L1, L2);
        pkt->header.senderId = 1;
        pkt->header.destId = 2;
        pkt->header.length = L;
        pkt->header.seqNum = seqNum;
        memset(pkt->payload, seqNum & 0xFF, L);

        xQueueSend(xGenQueue, &pkt, portMAX_DELAY);
        seqNum++;

        /* Inter-packet gap [T1,T2] ms */
        uint16_t gap = randRange(T1_MS, T2_MS);
        vTaskDelay(pdMS_TO_TICKS(gap));
    }
}

void vSenderTask(void *pvParameters)
{
    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        Packet_t *pkt = NULL;
        xQueueReceive(xGenQueue, &pkt, portMAX_DELAY);

        xSemaphoreTake(xStatsMutex, portMAX_DELAY);
        stats.sent++;
        xSemaphoreGive(xStatsMutex);

        uint8_t attempts = 0;
        uint8_t acked = 0;

        /* First transmit */
        Packet_t *txPkt = (Packet_t *)malloc(sizeof(Packet_t));
        configASSERT(txPkt);
        memcpy(txPkt, pkt, sizeof(Packet_t));
        xQueueSend(xTxQueue, &txPkt, portMAX_DELAY);
        attempts++;

        while (!acked && attempts <= MAX_RETX)
        {
            if (xSemaphoreTake(xAckSem, pdMS_TO_TICKS(TIMEOUT_MS)) == pdTRUE)
            {
                acked = 1;
            }
            else
            {
                /* Timeout */
                xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                stats.timeouts++;
                xSemaphoreGive(xStatsMutex);

                if (attempts < MAX_RETX)
                {
                    xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                    stats.retransmitted++;
                    xSemaphoreGive(xStatsMutex);

                    Packet_t *retxPkt = (Packet_t *)malloc(sizeof(Packet_t));
                    configASSERT(retxPkt);
                    memcpy(retxPkt, pkt, sizeof(Packet_t));
                    xQueueSend(xTxQueue, &retxPkt, portMAX_DELAY);
                }
                attempts++;
            }
        }

        if (!acked)
        {
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.droppedMaxRetx++;
            xSemaphoreGive(xStatsMutex);
        }

        free(pkt);
    }
}

void vLinkSimTask(void *pvParameters)
{
    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        Packet_t *pkt = NULL;
        xQueueReceive(xTxQueue, &pkt, portMAX_DELAY);

        /* Propagation + transmission delay */
        uint32_t delay = D_MS + txDelayMs(pkt->header.length);
        vTaskDelay(pdMS_TO_TICKS(delay));

        if (randFloat() < DROP_PROB)
        {
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.droppedByLink++;
            xSemaphoreGive(xStatsMutex);
            free(pkt);
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
    uint32_t expectedSeq = 0;

    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        Packet_t *pkt = NULL;
        xQueueReceive(xRxQueue, &pkt, portMAX_DELAY);

        if (pkt->header.seqNum == expectedSeq)
        {
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.received++;
            stats.bytesReceived += pkt->header.length;
            uint32_t rcvd = stats.received;
            xSemaphoreGive(xStatsMutex);

            /* Send ACK */
            Ack_t *ack = (Ack_t *)malloc(sizeof(Ack_t));
            configASSERT(ack);
            ack->srcNode = 2;
            ack->destNode = 1;
            ack->seqNum = pkt->header.seqNum;
            xQueueSend(xAckTxQueue, &ack, 0);

            expectedSeq++;
            free(pkt);

            if (rcvd >= TARGET)
            {
                clock_gettime(CLOCK_MONOTONIC, &tEnd);
                simulationDone = 1;
            }
        }
        else
        {
            /* Duplicate — discard */
            free(pkt);
        }
    }
}

void vAckLinkSimTask(void *pvParameters)
{
    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        Ack_t *ack = NULL;
        xQueueReceive(xAckTxQueue, &ack, portMAX_DELAY);

        /* ACK delay: propagation + fixed K bytes transmission */
        uint32_t delay = D_MS + txDelayMs(K);
        vTaskDelay(pdMS_TO_TICKS(delay));

        if (randFloat() < P_ACK)
        {
            /* ACK dropped */
            free(ack);
        }
        else
        {
            xQueueSend(xAckRxQueue, &ack, portMAX_DELAY);
        }
    }
}

void vAckRelayTask(void *pvParameters)
{
    for (;;)
    {
        if (simulationDone)
            vTaskDelete(NULL);

        Ack_t *ack = NULL;
        xQueueReceive(xAckRxQueue, &ack, portMAX_DELAY);
        free(ack);
        xSemaphoreGive(xAckSem);
    }
}

void vStatsPrinterTask(void *pvParameters)
{
    while (!simulationDone)
        vTaskDelay(pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(500));

    double wallSec = (tEnd.tv_sec - tStart.tv_sec) +
                     (tEnd.tv_nsec - tStart.tv_nsec) / 1e9;

    double throughput = (double)stats.bytesReceived / wallSec;

    xSemaphoreTake(xStatsMutex, portMAX_DELAY);
    printf("\n===== SIMULATION COMPLETE =====\n");
    printf("DROP_PROB=%.2f TIMEOUT_MS=%d\n", (double)DROP_PROB, TIMEOUT_MS);
    printf("Sent:              %u\n", stats.sent);
    printf("Received:          %u\n", stats.received);
    printf("Bytes received:    %llu\n", (unsigned long long)stats.bytesReceived);
    printf("Dropped by link:   %u\n", stats.droppedByLink);
    printf("Retransmitted:     %u\n", stats.retransmitted);
    printf("Timeouts:          %u\n", stats.timeouts);
    printf("Dropped (maxRetx): %u\n", stats.droppedMaxRetx);
    printf("Wall time (s):     %.2f\n", wallSec);
    printf("Throughput (B/s):  %.1f\n", throughput);
    printf("STATS: %u,%u,%llu,%u,%u,%u,%u,%.2f,%.1f\n",
           stats.sent, stats.received,
           (unsigned long long)stats.bytesReceived,
           stats.droppedByLink, stats.retransmitted,
           stats.timeouts, stats.droppedMaxRetx,
           wallSec, throughput);
    xSemaphoreGive(xStatsMutex);

    exit(0);
}

int main(void)
{
    srand((unsigned int)time(NULL));

    xGenQueue = xQueueCreate(20, sizeof(Packet_t *));
    xTxQueue = xQueueCreate(20, sizeof(Packet_t *));
    xRxQueue = xQueueCreate(20, sizeof(Packet_t *));
    xAckTxQueue = xQueueCreate(20, sizeof(Ack_t *));
    xAckRxQueue = xQueueCreate(20, sizeof(Ack_t *));
    xAckSem = xSemaphoreCreateBinary();
    xStatsMutex = xSemaphoreCreateMutex();

    configASSERT(xGenQueue);
    configASSERT(xTxQueue);
    configASSERT(xRxQueue);
    configASSERT(xAckTxQueue);
    configASSERT(xAckRxQueue);
    configASSERT(xAckSem);
    configASSERT(xStatsMutex);

    xTaskCreate(vPacketGeneratorTask, "Gen", 512, NULL, 1, NULL);
    xTaskCreate(vSenderTask, "Sender", 512, NULL, 3, NULL);
    xTaskCreate(vLinkSimTask, "Link", 512, NULL, 2, NULL);
    xTaskCreate(vReceiverTask, "RX", 512, NULL, 2, NULL);
    xTaskCreate(vAckLinkSimTask, "AckLink", 512, NULL, 2, NULL);
    xTaskCreate(vAckRelayTask, "AckRly", 512, NULL, 3, NULL);
    xTaskCreate(vStatsPrinterTask, "Stats", 512, NULL, 1, NULL);

    vTaskStartScheduler();
    return 0;
}