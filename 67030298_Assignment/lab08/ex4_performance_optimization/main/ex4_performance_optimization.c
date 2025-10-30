/*
 * Exercise 4: Performance Optimization and Monitoring
 *
 * 1. Comprehensive performance monitoring (Heap + Tasks)
 * 2. Optimize memory allocation (heap_caps_malloc)
 * 3. Measure/Benchmark (using esp_timer)
 * 4. Implement watchdog
 */

#include <stdio.h>
#include <string.h>
#include <math.h> // for benchmark
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h" // 2. Memory Allocation
#include "esp_task_wdt.h"  // 4. Watchdog
#include "esp_timer.h"     // 3. Benchmark

static const char *TAG = "EX4_PERFORMANCE";

// 4. Watchdog Timeout (วินาที)
#define WDT_TIMEOUT_S 5

/**
 * @brief 1. Task สำหรับ Monitor ระบบ
 * - แสดง Heap, Task List
 * - 4. Reset Watchdog
 */
void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Monitor Task: Started on Core %d", xPortGetCoreID());
    
    // 4. เพิ่ม Task นี้เข้า Watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    char buffer[2048]; // สำหรับ vTaskList

    while (1)
    {
        // 4. Reset Watchdog
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        ESP_LOGI(TAG, "============ SYSTEM MONITOR (Core %d) ============", xPortGetCoreID());

        // 1. Monitor Heap
        ESP_LOGI(TAG, "Heap Free: %d (Min: %d)",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        
        // 2. Monitor specific memory types
        ESP_LOGI(TAG, "Heap - Internal: %d bytes free", 
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Heap - DMA: %d bytes free", 
                 heap_caps_get_free_size(MALLOC_CAP_DMA));
        #if CONFIG_SPIRAM_SUPPORT
        ESP_LOGI(TAG, "Heap - SPIRAM: %d bytes free", 
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        #endif

        // 1. Monitor Tasks
        vTaskList(buffer);
        printf("Task List:\nName\t\tState\tPrio\tStack\tCore\n");
        printf("%s\n", buffer);

        ESP_LOGI(TAG, "=================================================");

        vTaskDelay(pdMS_TO_TICKS(3000)); // Monitor ทุก 3 วินาที
    }
}

/**
 * @brief 2. Task ทดสอบการจอง Memory
 * - จอง Memory ประเภทต่างๆ
 */
void memory_alloc_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Memory Alloc Task: Started on Core %d", xPortGetCoreID());

    while (1)
    {
        ESP_LOGI(TAG, "Memory Task: Allocating buffers...");

        // จอง DMA-capable memory
        uint8_t *dma_buffer = heap_caps_malloc(1024, MALLOC_CAP_DMA);
        if (dma_buffer) {
            ESP_LOGI(TAG, "Allocated 1024 bytes DMA buffer at %p", dma_buffer);
            heap_caps_free(dma_buffer);
        } else {
            ESP_LOGE(TAG, "Failed to allocate DMA buffer");
        }

        // จอง SPIRAM (ถ้ามี)
        #if CONFIG_SPIRAM_SUPPORT
        void *spiram_buffer = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
        if (spiram_buffer) {
            ESP_LOGI(TAG, "Allocated 32KB SPIRAM buffer at %p", spiram_buffer);
            heap_caps_free(spiram_buffer);
        } else {
            ESP_LOGE(TAG, "Failed to allocate SPIRAM buffer");
        }
        #endif

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 วินาที
    }
}

/**
 * @brief 3. Task สำหรับ Benchmark (จาก doc)
 * - ทำงานหนักๆ เพื่อวัดประสิทธิภาพ
 */
void benchmark_task(void *parameter)
{
    int core_id = (int)parameter;
    ESP_LOGI(TAG, "Benchmark Task: Started on Core %d", core_id);

    while(1)
    {
        int64_t start_time = esp_timer_get_time();
        
        // Simple computational benchmark
        volatile float result = 0;
        for (int i = 0; i < 50000; i++) {
            result += sqrtf(i * 3.14159f);
        }
        
        int64_t end_time = esp_timer_get_time();
        int64_t execution_time_us = end_time - start_time;

        ESP_LOGI(TAG, "Benchmark Core %d: Calculation took %lld µs", 
                 core_id, execution_time_us);
        
        vTaskDelay(pdMS_TO_TICKS(2000)); // พัก 2 วินาที
    }
}

/**
 * @brief 4. Task จำลองการค้าง (Stuck)
 * - เพื่อทดสอบ Watchdog
 */
void stuck_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stuck Task: Started on Core %d", xPortGetCoreID());
    
    // 4. เพิ่ม Task นี้เข้า Watchdog
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    // ทำงานปกติ 15 วินาที
    for(int i = 0; i < 15; i++) {
        ESP_LOGW(TAG, "Stuck Task: Feeding WDT... (%d/15)", i+1);
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // จำลองการค้าง
    ESP_LOGE(TAG, "Stuck Task: STUCK in infinite loop (WDT will trigger in %d s)", WDT_TIMEOUT_S);
    while(1) {
        // ไม่ reset WDT
    }
}


void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 4: Performance and Monitoring");

    // 4. ตั้งค่า Task Watchdog Timer
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1), // Watch IDLE tasks on Core 0 and 1
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config)); // Panic on timeout

    // 1. สร้าง Monitor Task (Core 1)
    xTaskCreatePinnedToCore(
        system_monitor_task,
        "MonitorTask",
        4096,
        NULL,
        5,
        NULL,
        APP_CPU_NUM
    );

    // 2. สร้าง Memory Alloc Task (No Affinity)
    xTaskCreate(
        memory_alloc_task,
        "MemoryTask",
        4096,
        NULL,
        5,
        NULL
    );

    // 3. สร้าง Benchmark Tasks (Core 0 และ 1)
    xTaskCreatePinnedToCore(benchmark_task, "Bench0", 2048, (void*)0, 10, NULL, 0);
    xTaskCreatePinnedToCore(benchmark_task, "Bench1", 2048, (void*)1, 10, NULL, 1);

    // 4. สร้าง Stuck Task (Core 0)
    // *** เอา Comment ออก ถ้าต้องการทดสอบ WDT Panic ***
    /*
    xTaskCreatePinnedToCore(
        stuck_task,
        "StuckTask",
        2048,
        NULL,
        15, // Prio สูง
        NULL,
        PRO_CPU_NUM
    );
    */
}
