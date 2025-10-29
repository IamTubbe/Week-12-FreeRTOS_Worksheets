/*
 * Lab 3: Stack Monitoring (Combined Version)
 * ประกอบด้วยฟังก์ชันจาก Step 1, 2, 3 และ Exercises 1, 2
 * app_main ถูกตั้งค่าให้รันเวอร์ชัน Step 3 + Exercise 2 ที่เสถียร
 */

#include <stdio.h>
#include <stdlib.h> // สำหรับ malloc/free
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"  // สำหรับ esp_get_free_heap_size
#include "esp_rom_sys.h" // สำหรับ esp_rom_delay_us
#include "rom/uart.h"    // สำหรับ ets_printf

// --- Defines ---
#define LED_OK GPIO_NUM_2       // Stack OK indicator
#define LED_WARNING GPIO_NUM_4  // Stack warning indicator

static const char *TAG = "STACK_MONITOR_LAB"; // TAG หลัก

// Stack monitoring configuration
#define STACK_WARNING_THRESHOLD 512  // bytes
#define STACK_CRITICAL_THRESHOLD 256 // bytes

// --- Task Handles (Global) ---
TaskHandle_t light_task_handle = NULL;
TaskHandle_t medium_task_handle = NULL;
TaskHandle_t heavy_task_handle = NULL; // จะชี้ไปที่ OptimizedHeavy
TaskHandle_t recursion_task_handle = NULL; // สำหรับ Recursion task

// --- Stack Overflow Hook (Step 2 - Minimal Version) ---
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    // ใช้ ets_printf เพราะปลอดภัยกว่า ESP_LOGE ในสถานะ Panic
    ets_printf("!!! STACK OVERFLOW DETECTED in task '%s' !!!\n", pcTaskName);
    esp_rom_delay_us(100000); // Delay สั้นๆ
    esp_restart();
}

// --- Exercise 2 Function (Corrected) ---
// ฟังก์ชันสำหรับ Exercise 2 (แก้ไขให้รับ Pointer เพื่อเก็บค่าแยก Task)
void dynamic_stack_monitor(TaskHandle_t task_handle, const char* task_name, UBaseType_t *previous_remaining_ptr)
{
    UBaseType_t current_remaining = uxTaskGetStackHighWaterMark(task_handle);

    if (previous_remaining_ptr != NULL && *previous_remaining_ptr != 0) {
        if (current_remaining < *previous_remaining_ptr) {
            ESP_LOGW(TAG, "%s stack usage increased (remaining decreased by %lu bytes)",
                     task_name,
                     (*previous_remaining_ptr - current_remaining) * sizeof(StackType_t));
        }
    }
    // อัปเดตค่าเก่าผ่าน Pointer
    if (previous_remaining_ptr != NULL) {
        *previous_remaining_ptr = current_remaining;
    }
}

// --- Task Functions (Step 1 + Exercise 2 modifications) ---

// Stack monitoring task (Modified for Exercise 2)
void stack_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stack Monitor Task started");

    // Array สำหรับเก็บค่า Stack remaining ก่อนหน้า (สำหรับ Exercise 2)
    // ขนาดต้องเท่ากับจำนวน Task ที่ Monitor (5 Tasks)
    static UBaseType_t previous_remaining[5] = {0};

    while (1) {
        ESP_LOGI(TAG, "\n--- STACK USAGE REPORT ---");

        TaskHandle_t tasks[] = {
            light_task_handle,
            medium_task_handle,
            heavy_task_handle,      // ชี้ไปที่ OptimizedHeavy
            recursion_task_handle,  // Monitor task นี้ด้วย
            xTaskGetCurrentTaskHandle() // Monitor ตัวเอง
        };
        const int num_tasks_to_monitor = sizeof(tasks) / sizeof(tasks[0]);
        const char* task_names[num_tasks_to_monitor];

        bool stack_warning = false;
        bool stack_critical = false;

        for (int i = 0; i < num_tasks_to_monitor; i++) {
            if (tasks[i] != NULL) {
                task_names[i] = pcTaskGetName(tasks[i]);
                UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(tasks[i]);
                uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
                ESP_LOGI(TAG, "%s: %lu bytes remaining", task_names[i] ? task_names[i] : "Task(NULL)", stack_bytes);

                // --- Call Exercise 2 Function ---
                dynamic_stack_monitor(tasks[i], task_names[i] ? task_names[i] : "Task(NULL)", &previous_remaining[i]);

                if (tasks[i] != xTaskGetCurrentTaskHandle()) {
                    if (stack_bytes < STACK_CRITICAL_THRESHOLD) {
                        ESP_LOGE(TAG, "CRITICAL: %s stack very low!", task_names[i] ? task_names[i] : "Task(NULL)");
                        stack_critical = true;
                    } else if (stack_bytes < STACK_WARNING_THRESHOLD) {
                        ESP_LOGW(TAG, "WARNING: %s stack low", task_names[i] ? task_names[i] : "Task(NULL)");
                        stack_warning = true;
                    }
                }
            } else {
                task_names[i] = "Task (NULL)"; // Handle กรณี Task ยังไม่ถูกสร้าง
            }
        }

        // Update LED indicators
        if (stack_critical) {
            // Blink warning LED rapidly
            for (int i = 0; i < 10; i++) {
                gpio_set_level(LED_WARNING, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(LED_WARNING, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            gpio_set_level(LED_OK, 0);
        } else if (stack_warning) {
            gpio_set_level(LED_WARNING, 1);
            gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1);
            gpio_set_level(LED_WARNING, 0);
        }

        ESP_LOGI(TAG, "Free heap: %d bytes", (int)esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap: %d bytes", (int)esp_get_minimum_free_heap_size());
        ESP_LOGI(TAG, "--------------------------\n");

        vTaskDelay(pdMS_TO_TICKS(3000)); // Monitor every 3 seconds
    }
}

// Light task (Step 1)
void light_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Light Stack Task started (minimal usage)");
    int counter = 0;
    while (1) {
        counter++;
        ESP_LOGI(TAG, "Light task cycle: %d", counter);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// Medium task (Step 1)
void medium_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Stack Task started (moderate usage)");
    while (1) {
        char buffer[256];
        int numbers[50];
        memset(buffer, 'A', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        for (int i = 0; i < 50; i++) { numbers[i] = i * i; }
        ESP_LOGI(TAG, "Medium task: buffer[0]=%c, numbers[49]=%d", buffer[0], numbers[49]);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// --- Step 1 Original Heavy Task (DEFINED but NOT USED in app_main) ---
// (ฟังก์ชันนี้มีไว้เพื่อให้ Exercise 1 เรียกใช้ได้)
void heavy_stack_task(void *pvParameters)
{
    ESP_LOGI(pcTaskGetName(NULL), "Task started (high usage)");
    int cycle = 0;
    while (1) {
        cycle++;
        char large_buffer[1024];
        int large_numbers[200];
        char another_buffer[512];
        volatile char* p_lb = large_buffer;
        volatile int* p_ln = large_numbers;
        volatile char* p_ab = another_buffer;

        ESP_LOGI(pcTaskGetName(NULL), "Cycle %d: using stack arrays...", cycle);
        memset(large_buffer, 'H', sizeof(large_buffer));
        memset(large_numbers, cycle % 100, sizeof(large_numbers));
        snprintf(another_buffer, sizeof(another_buffer), "Heavy cycle %d", cycle);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(pcTaskGetName(NULL), "Stack remaining: %lu bytes", stack_remaining * sizeof(StackType_t));

        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

// --- Recursive function (Step 1) ---
void recursive_function(int depth, char *buffer)
{
    char local_array[100];
    snprintf(local_array, sizeof(local_array), "Recursion depth: %d", depth);
    ESP_LOGI(TAG, "%s", local_array);
    UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
    uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
    ESP_LOGI(TAG, "Depth %d: Stack remaining: %lu bytes", depth, stack_bytes);

    if (stack_bytes < 200) { // Safety stop
        ESP_LOGE(TAG, "Stopping recursion at depth %d - stack too low!", depth);
        return;
    }
    if (depth < 20) {  // Limit recursion
        vTaskDelay(pdMS_TO_TICKS(500));
        recursive_function(depth + 1, buffer);
    }
}

// --- Recursion demo task (Step 1) ---
void recursion_demo_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Recursion Demo Task started");
    while (1) {
        ESP_LOGW(TAG, "=== STARTING RECURSION DEMO ===");
        char shared_buffer[200]; // This array is on this task's stack
        recursive_function(1, shared_buffer);
        ESP_LOGW(TAG, "=== RECURSION DEMO COMPLETED ===");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// --- Optimized Heavy Task (Step 3) ---
void optimized_heavy_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Optimized Heavy Task started");
    char *large_buffer = malloc(1024);
    int *large_numbers = malloc(200 * sizeof(int));
    char *another_buffer = malloc(512);

    if (!large_buffer || !large_numbers || !another_buffer) {
        ESP_LOGE(TAG, "OptimizedHeavy: Failed to allocate heap memory!");
        free(large_buffer);
        free(large_numbers);
        free(another_buffer);
        vTaskDelete(NULL);
        return;
    } else {
         ESP_LOGI(TAG, "OptimizedHeavy: Heap buffers allocated successfully.");
    }

    int cycle = 0;
    while (1) {
        cycle++;
        ESP_LOGI(TAG, "Optimized task cycle %d: Using heap instead of stack", cycle);
        memset(large_buffer, 'Y', 1023);
        large_buffer[1023] = '\0';
        for (int i = 0; i < 200; i++) { large_numbers[i] = i * cycle; }
        snprintf(another_buffer, 512, "Optimized cycle %d", cycle);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Optimized task stack: %lu bytes remaining", stack_remaining * sizeof(StackType_t));
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
    // Cleanup (unreachable)
    free(large_buffer);
    free(large_numbers);
    free(another_buffer);
}

// --- Exercise 1 Function (DEFINED but NOT USED in app_main) ---
void test_stack_sizes(void)
{
    ESP_LOGI(TAG, "Starting stack size test...");
    uint32_t test_sizes[] = {512, 1024, 2048, 4096};
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        char task_name[20];
        snprintf(task_name, sizeof(task_name), "Test%lu", test_sizes[i]);
        BaseType_t result = xTaskCreate(heavy_stack_task, task_name, test_sizes[i], NULL, 1, NULL);
        ESP_LOGI(TAG, "Task '%s' with %lu bytes stack: %s",
                 task_name, test_sizes[i], result == pdPASS ? "Created successfully" : "FAILED to create");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
     ESP_LOGI(TAG, "Stack size test finished creating tasks.");
}

// --- app_main (Runs Step 3 + Exercise 2) ---
void app_main(void)
{
    ESP_LOGI(TAG, "=== Lab 3: Stack Monitoring (Combined Version) ===");

    // Config GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_OK) | (1ULL << LED_WARNING),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_OK, 0);
    gpio_set_level(LED_WARNING, 0);

    ESP_LOGI(TAG, "LEDs: GPIO%d=OK(Green), GPIO%d=Warning(Red)", LED_OK, LED_WARNING);
    ESP_LOGI(TAG, "Creating tasks for final demo (Step 3 + Ex 2)...");

    BaseType_t result;

    // เพิ่ม Stack ให้ LightTask (แก้ปัญหา Critical ที่เจอ)
    result = xTaskCreate(light_stack_task, "LightTask", 3072, NULL, 2, &light_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create light task");

    result = xTaskCreate(medium_stack_task, "MediumTask", 3072, NULL, 2, &medium_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create medium task");

    // ใช้ Optimized task (Step 3)
    result = xTaskCreate(optimized_heavy_task, "OptimizedHeavy", 3072, NULL, 2, &heavy_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create optimized heavy task");

    // เพิ่ม Recursion Task กลับมา (เพิ่ม Stack)
    result = xTaskCreate(recursion_demo_task, "RecursionDemo", 4096, NULL, 1, &recursion_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create recursion demo task");

    // Monitor task (Exercise 2)
    result = xTaskCreate(stack_monitor_task, "StackMonitor", 4096, NULL, 3, NULL);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create stack monitor task");

    // --- Task ที่ไม่ได้เรียกใช้ใน app_main นี้ ---
    // heavy_stack_task (ถูกแทนที่ด้วย optimized_heavy_task)
    // test_stack_sizes (เป็นฟังก์ชันทดสอบแยก)

    ESP_LOGI(TAG, "All tasks created. Monitoring stack usage changes...");
}
