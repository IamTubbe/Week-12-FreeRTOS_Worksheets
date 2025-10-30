/*
 * Exercise 1: Dual-Core Task Distribution
 *
 * 1. Compute-intensive task (Core 0)
 * 2. I/O task (Core 1)
 * 3. Inter-core communication (Queue)
 * 4. Monitor CPU utilization
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "EX1_DUAL_CORE";

// 3. Queue สำหรับสื่อสารระหว่าง Core
static QueueHandle_t data_queue;

/**
 * @brief 1. Task ประมวลผลหนักๆ (Compute-intensive)
 * - ปักหมุดที่ Core 0 (PRO_CPU)
 * - ทำการคำนวณ (จำลอง) และส่งผลลัพธ์ไปยัง Queue
 */
void compute_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Compute Task: Started on Core %d", xPortGetCoreID());
    uint32_t iteration = 0;

    while (1)
    {
        // จำลองการคำนวณที่ซับซ้อน
        volatile uint64_t calc = 0;
        for (int i = 0; i < 1000000; i++)
        {
            calc += i;
        }

        iteration++;

        // 3. ส่งข้อมูลไปให้ Core 1
        if (xQueueSend(data_queue, &iteration, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Compute Task: Queue full");
        }

        // หน่วงเวลาเล็กน้อยเพื่อให้ Task อื่นได้ทำงานบ้าง
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief 2. Task สำหรับ I/O (Logging/Communication)
 * - ปักหมุดที่ Core 1 (APP_CPU)
 * - รอรับข้อมูลจาก Queue และแสดงผล
 */
void io_task(void *pvParameters)
{
    ESP_LOGI(TAG, "I/O Task: Started on Core %d", xPortGetCoreID());
    uint32_t received_iteration = 0;

    while (1)
    {
        // 3. รอรับข้อมูลจาก Core 0
        if (xQueueReceive(data_queue, &received_iteration, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGI(TAG, "I/O Task (Core %d): Received compute iteration %d",
                     xPortGetCoreID(), received_iteration);
        }
    }
}

/**
 * @brief 4. Task สำหรับ Monitor CPU
 * - ไม่ปักหมุด (No Affinity)
 * - แสดงผล Task List และ Heap ทุก 10 วินาที
 *
 * @note ต้องเปิดใช้งานใน menuconfig:
 * Component config -> FreeRTOS -> Enable FreeRTOS trace facility
 * Component config -> FreeRTOS -> Enable FreeRTOS stats formatting functions
 */
void monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Monitor Task: Started on Core %d", xPortGetCoreID());
    char buffer[2048]; // Buffer สำหรับ vTaskList

    while (1)
    {
        ESP_LOGI(TAG, "============ SYSTEM MONITOR (Core %d) ============", xPortGetCoreID());

        // แสดงผล Task List
        vTaskList(buffer);
        printf("Task List:\nName\t\tState\tPrio\tStack\tCore\n");
        printf("%s\n", buffer);

        // แสดงผล Heap
        ESP_LOGI(TAG, "Heap Free: %d bytes (Min: %d bytes)",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

        ESP_LOGI(TAG, "=================================================");

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 วินาที
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 1: Dual-Core Distribution");

    // 3. สร้าง Queue
    data_queue = xQueueCreate(10, sizeof(uint32_t));

    // 1. สร้าง Task ประมวลผล (Core 0)
    xTaskCreatePinnedToCore(
        compute_task,   // Function
        "ComputeTask",  // Name
        4096,           // Stack size
        NULL,           // Parameters
        10,             // Priority
        NULL,           // Handle
        PRO_CPU_NUM     // Core 0
    );

    // 2. สร้าง Task I/O (Core 1)
    xTaskCreatePinnedToCore(
        io_task,        // Function
        "IOTask",       // Name
        4096,           // Stack size
        NULL,           // Parameters
        10,             // Priority
        NULL,           // Handle
        APP_CPU_NUM     // Core 1
    );

    // 4. สร้าง Task Monitor (No Affinity)
    xTaskCreate(
        monitor_task,   // Function
        "MonitorTask",  // Name
        4096,           // Stack size
        NULL,           // Parameters
        5,              // Priority
        NULL            // Handle
    );
}
