/*
 * Exercise 2: Core-Pinned Real-Time System
 *
 * 1. High-frequency control task (1kHz) on Core 0
 * 2. Data acquisition task (500Hz) on Core 0
 * 3. Communication task on Core 1
 * 4. Background processing with no affinity
 * 5. Measure timing precision and jitter
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h" // สำหรับ esp_timer_get_time()

static const char *TAG = "EX2_REALTIME";

// 1. High-frequency control task (1kHz) - Core 0
void control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Control Task (1kHz): Started on Core %d [Prio %d]",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    const TickType_t xFrequency = pdMS_TO_TICKS(1); // 1 ms = 1kHz
    TickType_t xLastWakeTime = xTaskGetTickCount();
    int64_t last_time_us = esp_timer_get_time();

    while (1)
    {
        // 5. วัด Jitter
        int64_t now_us = esp_timer_get_time();
        int64_t jitter_us = (now_us - last_time_us) - 1000; // 1000 us = 1ms
        last_time_us = now_us;

        // จำลองการทำงาน (ต้องสั้นมากๆ)
        // if (jitter_us > 50 || jitter_us < -50) { // Log เฉพาะเมื่อ Jitter สูง
        //     ESP_LOGW(TAG, "Control Task Jitter: %lld us", jitter_us);
        // }

        // รอคาบเวลาถัดไป
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// 2. Data acquisition task (500Hz) - Core 0
void data_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data Task (500Hz): Started on Core %d [Prio %d]",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    const TickType_t xFrequency = pdMS_TO_TICKS(2); // 2 ms = 500Hz
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1)
    {
        // จำลองการอ่าน Sensor
        // ESP_LOGI(TAG, "Data Task: Reading data...");
        vTaskDelay(pdMS_TO_TICKS(1)); // จำลองว่าใช้เวลาอ่าน 1ms

        // รอคาบเวลาถัดไป
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// 3. Communication task - Core 1
void comm_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Comm Task: Started on Core %d [Prio %d]",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    while (1)
    {
        ESP_LOGI(TAG, "Comm Task (Core %d): Sending data to cloud...", xPortGetCoreID());
        // จำลองการส่งข้อมูล (เช่น WiFi/BLE) ซึ่งมักใช้เวลานาน
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 4. Background processing - No Affinity
void background_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Background Task: Started on Core %d [Prio %d]",
             xPortGetCoreID(), uxTaskPriorityGet(NULL));

    while (1)
    {
        ESP_LOGI(TAG, "Background Task (Core %d): Doing background work...", xPortGetCoreID());
        // งานที่ไม่สำคัญ เช่น log file, UI update
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 2: Core-Pinned Real-Time System");

    // 1. High-frequency control task (Core 0, Prio สูงสุด)
    xTaskCreatePinnedToCore(
        control_task,
        "ControlTask",
        2048,
        NULL,
        20, // Prio สูง
        NULL,
        PRO_CPU_NUM
    );

    // 2. Data acquisition task (Core 0, Prio รองลงมา)
    xTaskCreatePinnedToCore(
        data_task,
        "DataTask",
        2048,
        NULL,
        18, // Prio รอง
        NULL,
        PRO_CPU_NUM
    );

    // 3. Communication task (Core 1)
    xTaskCreatePinnedToCore(
        comm_task,
        "CommTask",
        4096, // งาน network มักใช้ stack เยอะ
        NULL,
        15, // Prio กลาง
        NULL,
        APP_CPU_NUM
    );

    // 4. Background processing (No Affinity, Prio ต่ำสุด)
    xTaskCreate(
        background_task,
        "BackgroundTask",
        2048,
        NULL,
        5, // Prio ต่ำ
        NULL
    );
}
