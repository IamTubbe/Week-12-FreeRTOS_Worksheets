#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h" // เพิ่ม include สำหรับ esp_get_free_heap_size() และอื่นๆ
#include <stdlib.h>     // เพิ่ม include สำหรับ malloc/free ใน runtime_stats_task
#include <string.h>     // เพิ่ม include สำหรับ memset ใน runtime_stats_task (ถ้าใช้)

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4

static const char *TAG = "BASIC_TASKS";

// --- Function Prototypes (Declarations) ---
// ประกาศฟังก์ชันไว้ก่อน เพื่อให้ app_main รู้จัก
void led1_task(void *pvParameters);
void led2_task(void *pvParameters);
void system_info_task(void *pvParameters);

// --- Prototype for Step 2 ---
void task_manager(void *pvParameters);

// --- Prototypes for Step 3 ---
void high_priority_task(void *pvParameters);
void low_priority_task(void *pvParameters);
void runtime_stats_task(void *pvParameters);

void temporary_task(void *pvParameters); // <--- เพิ่มบรรทัดนี้สำหรับ Exercise 1
void producer_task(void *pvParameters); // <--- เพิ่มบรรทัดนี้สำหรับ Exercise 2
void consumer_task(void *pvParameters); // <--- เพิ่มบรรทัดนี้สำหรับ Exercise 2

// --- Task function สำหรับ LED1 (Step 1) ---
void led1_task(void *pvParameters)
{
    // Cast parameter ถ้ามี
    int *task_id = (int *)pvParameters;

    ESP_LOGI(TAG, "LED1 Task started with ID: %d", *task_id);

    // Task loop - ต้องมี infinite loop
    while (1) {
        ESP_LOGI(TAG, "LED1 ON");
        gpio_set_level(LED1_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500)); // 500ms delay

        ESP_LOGI(TAG, "LED1 OFF");
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500)); // 500ms delay
    }

    // หมายเหตุ: จุดนี้จะไม่เคยถูกเรียก เพราะ infinite loop
    ESP_LOGI(TAG, "LED1 Task ended"); // จะไม่ถูกเรียก
    vTaskDelete(NULL); // Delete ตัวเอง
}

// --- Task function สำหรับ LED2 (Step 1) ---
void led2_task(void *pvParameters)
{
    char *task_name = (char *)pvParameters;

    ESP_LOGI(TAG, "LED2 Task started: %s", task_name);

    while (1) {
        ESP_LOGI(TAG, "LED2 Blink Fast");

        // Fast blink pattern
        for (int i = 0; i < 5; i++) {
            gpio_set_level(LED2_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED2_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Pause between patterns
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- Task สำหรับแสดงสถิติระบบ (Step 1) ---
void system_info_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System Info Task started");

    while (1) {
        // แสดงข้อมูลระบบ
        ESP_LOGI(TAG, "=== System Information ===");
        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap: %d bytes", esp_get_minimum_free_heap_size());

        // แสดงจำนวน tasks ที่กำลังทำงาน
        UBaseType_t task_count = uxTaskGetNumberOfTasks();
        ESP_LOGI(TAG, "Number of tasks: %d", task_count);

        // แสดง uptime
        TickType_t uptime = xTaskGetTickCount();
        uint32_t uptime_sec = uptime * portTICK_PERIOD_MS / 1000;
        ESP_LOGI(TAG, "Uptime: %d seconds", uptime_sec);

        vTaskDelay(pdMS_TO_TICKS(3000)); // รายงานทุก 3 วินาที
    }
}


// --- START: Step 2 Code ---
// /*
// Task สำหรับควบคุม tasks อื่นๆ
void task_manager(void *pvParameters)
{
    ESP_LOGI(TAG, "Task Manager started");

    TaskHandle_t *handles = (TaskHandle_t *)pvParameters;
    TaskHandle_t led1_handle = handles[0];
    TaskHandle_t led2_handle = handles[1];

    int command_counter = 0;

    while (1) {
        command_counter++;

        // ตรวจสอบว่า handle ไม่ใช่ NULL ก่อนใช้งาน (เพิ่มความปลอดภัย)
        if (led1_handle == NULL || led2_handle == NULL) {
             ESP_LOGE(TAG, "Task Manager: Invalid task handles received!");
             vTaskDelay(pdMS_TO_TICKS(5000)); // รอสักครู่แล้วลองใหม่ หรือจัดการ error
             continue;
        }


        switch (command_counter % 6) {
            case 1:
                ESP_LOGI(TAG, "Manager: Suspending LED1");
                vTaskSuspend(led1_handle);
                break;

            case 2:
                ESP_LOGI(TAG, "Manager: Resuming LED1");
                vTaskResume(led1_handle);
                break;

            case 3:
                ESP_LOGI(TAG, "Manager: Suspending LED2");
                vTaskSuspend(led2_handle);
                break;

            case 4:
                ESP_LOGI(TAG, "Manager: Resuming LED2");
                vTaskResume(led2_handle);
                break;

            case 5:
                ESP_LOGI(TAG, "Manager: Getting task info");
                // ใช้ eTaskState แทน enum ตรงๆ เพื่อความเข้ากันได้
                eTaskState led1_state = eTaskGetState(led1_handle);
                eTaskState led2_state = eTaskGetState(led2_handle);
                ESP_LOGI(TAG, "LED1 State: %s",
                         (led1_state == eRunning) ? "Running" :
                         (led1_state == eReady) ? "Ready" :
                         (led1_state == eBlocked) ? "Blocked" :
                         (led1_state == eSuspended) ? "Suspended" : "Deleted/Invalid");
                ESP_LOGI(TAG, "LED2 State: %s",
                         (led2_state == eRunning) ? "Running" :
                         (led2_state == eReady) ? "Ready" :
                         (led2_state == eBlocked) ? "Blocked" :
                         (led2_state == eSuspended) ? "Suspended" : "Deleted/Invalid");
                break;

            case 0:
                ESP_LOGI(TAG, "Manager: Reset cycle");
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // 2 seconds
    }
}
// */
// --- END: Step 2 Code ---


// --- START: Step 3 Code ---

// Task สำหรับทดสอบ priorities
void high_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "High Priority Task started");

    while (1) {
        ESP_LOGW(TAG, "HIGH PRIORITY TASK RUNNING!");

        // Simulate high priority work
        for (int i = 0; i < 1000000; i++) {
            volatile int dummy = i;
        }

        ESP_LOGW(TAG, "High priority task yielding");
        vTaskDelay(pdMS_TO_TICKS(5000)); // 5 seconds
    }
}

void low_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Low Priority Task started");

    while (1) {
        ESP_LOGI(TAG, "Low priority task running");

        // This task will be preempted by higher priority tasks
        for (int i = 0; i < 100; i++) {
            ESP_LOGI(TAG, "Low priority work: %d/100", i+1);
            vTaskDelay(pdMS_TO_TICKS(100)); // ให้ Task อื่นมีโอกาสทำงาน
        }
        // ไม่จำเป็นต้องมี Delay ตรงนี้อีก เพราะมีใน loop ด้านบนแล้ว
        // vTaskDelay(pdMS_TO_TICKS(100)); // ลบบรรทัดนี้ออกก็ได้
    }
}

// Task สำหรับแสดง runtime statistics
// ต้องเปิดใช้งาน Runtime Stats ใน menuconfig ก่อน:
// Component config -> FreeRTOS -> Enable FreeRTOS stats formatting functions
// Component config -> FreeRTOS -> Enable FreeRTOS trace facility -> Enable Trace Facility
// Component config -> FreeRTOS -> Enable FreeRTOS trace facility -> Enable Run Time Stats collection
// Component config -> FreeRTOS -> Enable FreeRTOS trace facility -> Choose the clock source for run time stats (ESP Timer)
void runtime_stats_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Runtime Stats Task started");

    // Allocate buffer for runtime stats (ควรใหญ่พอ)
    char *buffer = malloc(2048); // เพิ่มขนาด buffer
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for runtime stats");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        ESP_LOGI(TAG, "\n=== Runtime Statistics ===");

        // Get runtime statistics
        vTaskGetRunTimeStats(buffer); // ฟังก์ชันนี้จะเขียนข้อมูลลง buffer
        // ESP_LOGI(TAG, "Task\t\tAbs Time\tPercent Time"); // Header นี้อาจไม่ตรงกับ output จริง
        printf("--- Runtime Stats ---\n"); // ใช้ printf เพื่อแสดงผล buffer ตรงๆ
        printf("%s", buffer);
        printf("---------------------\n");


        // Get task list (Optional, อาจแสดงข้อมูลซ้ำซ้อนกับ Runtime Stats)
        // ESP_LOGI(TAG, "\n=== Task List ===");
        // vTaskList(buffer);
        // ESP_LOGI(TAG, "Name\t\tState\tPriority\tStack\tNum");
        // ESP_LOGI(TAG, "%s", buffer);

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds
    }

    free(buffer); // ควรจะมาไม่ถึงตรงนี้ แต่ใส่ไว้เผื่อ
}

// --- END: Step 3 Code ---


// --- ฟังก์ชันหลัก app_main ---
void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Basic Tasks Demo ===");

    // GPIO Configuration
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // Parameters สำหรับ tasks
    static int led1_id = 1;
    static char led2_name[] = "FastBlinker";

    // สร้าง task handles (ประกาศที่นี่เพื่อให้มี scope ครอบคลุม)
    TaskHandle_t led1_handle = NULL;
    TaskHandle_t led2_handle = NULL;
    TaskHandle_t info_handle = NULL;

    // --- สร้าง Task สำหรับ Exercise 1 ---
    static int temp_duration = 10; // กำหนดเวลาทำงาน 10 วินาที
    BaseType_t result_ex1 = xTaskCreate(
        temporary_task,     // ชื่อฟังก์ชัน Task
        "TempTask",         // ชื่อ Task
        2048,               // Stack size
        &temp_duration,     // Parameter (เวลาทำงาน)
        1,                  // Priority
        NULL                // Task handle (ไม่ต้องการ)
    );
    if (result_ex1 == pdPASS) {
        ESP_LOGI(TAG, "Temporary Task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create Temporary Task");
    }
    // --- สิ้นสุดการสร้าง Task Exercise 1 ---

    // --- สร้าง Tasks สำหรับ Exercise 2 ---
    BaseType_t result_ex2_prod = xTaskCreate(
        producer_task,      // ชื่อฟังก์ชัน Task
        "Producer",         // ชื่อ Task
        2048,               // Stack size
        NULL,               // Parameter
        2,                  // Priority
        NULL                // Task handle
    );
     if (result_ex2_prod != pdPASS) ESP_LOGE(TAG, "Failed to create Producer Task"); else ESP_LOGI(TAG, "Producer Task created");

    BaseType_t result_ex2_cons = xTaskCreate(
        consumer_task,      // ชื่อฟังก์ชัน Task
        "Consumer",         // ชื่อ Task
        2048,               // Stack size
        NULL,               // Parameter
        2,                  // Priority
        NULL                // Task handle
    );
     if (result_ex2_cons != pdPASS) ESP_LOGE(TAG, "Failed to create Consumer Task"); else ESP_LOGI(TAG, "Consumer Task created");
    // --- สิ้นสุดการสร้าง Tasks Exercise 2 ---

    // --- Task Creation (Step 1) ---
    BaseType_t result1 = xTaskCreate(led1_task, "LED1_Task", 2048, &led1_id, 2, &led1_handle);
    if (result1 != pdPASS) ESP_LOGE(TAG, "Failed to create LED1 Task"); else ESP_LOGI(TAG, "LED1 Task created");

    BaseType_t result2 = xTaskCreate(led2_task, "LED2_Task", 2048, led2_name, 2, &led2_handle);
    if (result2 != pdPASS) ESP_LOGE(TAG, "Failed to create LED2 Task"); else ESP_LOGI(TAG, "LED2 Task created");

    BaseType_t result3 = xTaskCreate(system_info_task, "SysInfo_Task", 3072, NULL, 1, &info_handle);
    if (result3 != pdPASS) ESP_LOGE(TAG, "Failed to create System Info Task"); else ESP_LOGI(TAG, "System Info Task created");


    // --- Task Creation (Step 2) --- Uncomment block นี้เพื่อทดสอบ Step 2

    TaskHandle_t task_handles[2] = {NULL, NULL};
    if (led1_handle) task_handles[0] = led1_handle;
    if (led2_handle) task_handles[1] = led2_handle;

    BaseType_t result4 = xTaskCreate(task_manager, "TaskManager", 2048, task_handles, 3, NULL);
    if (result4 != pdPASS) ESP_LOGE(TAG, "Failed to create Task Manager"); else ESP_LOGI(TAG, "Task Manager created");


    // --- Task Creation (Step 3) --- Uncomment block นี้เพื่อทดสอบ Step 3

    // อาจต้อง config runtime stats ใน menuconfig ก่อน
    // Component config -> FreeRTOS -> Enable FreeRTOS stats formatting functions
    // Component config -> FreeRTOS -> Enable FreeRTOS trace facility -> Enable Trace Facility & Run Time Stats collection & Choose clock source

    xTaskCreate(high_priority_task, "HighPri", 2048, NULL, 5, NULL); // Priority สูง
    xTaskCreate(low_priority_task, "LowPri", 2048, NULL, 1, NULL);  // Priority ต่ำ (อาจไม่ค่อยได้รันเมื่อ high_priority_task ทำงานหนัก)
    xTaskCreate(runtime_stats_task, "StatsTask", 4096, NULL, 1, NULL); // Stack ใหญ่หน่อยสำหรับ stats



    ESP_LOGI(TAG, "All tasks specified have been created (check comments). Main task will now idle.");
    ESP_LOGI(TAG, "Task handles - LED1: %p, LED2: %p, Info: %p",
             led1_handle, led2_handle, info_handle);

    // Main task can continue or be deleted
    // Option 1: Keep main task running
    while (1) {
        ESP_LOGD(TAG, "Main task heartbeat"); // ลดระดับ Log เป็น Debug
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds
    }

    // Option 2: Delete main task (uncomment below)
    // vTaskDelete(NULL);

}

// --- START: Exercise 1 Code ---
void temporary_task(void *pvParameters)
{
    int *duration = (int *)pvParameters;

    ESP_LOGI(TAG, "Temporary task will run for %d seconds", *duration);

    for (int i = *duration; i > 0; i--) {
        ESP_LOGI(TAG, "Temporary task countdown: %d", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Temporary task self-deleting");
    vTaskDelete(NULL); // ลบ Task ตัวเอง
}
// --- END: Exercise 1 Code ---

// --- START: Exercise 2 Code ---
// Global variable สำหรับการสื่อสาร (วิธีพื้นฐาน, ไม่แนะนำสำหรับงานจริง)
volatile int shared_counter = 0;

void producer_task(void *pvParameters)
{
    // Use TAG defined earlier or define a new one if needed
    // static const char *TAG_EX2 = "EXERCISE_2";
    ESP_LOGI(TAG, "Producer Task started");
    while (1) {
        shared_counter++; // เพิ่มค่า counter
        ESP_LOGI(TAG, "Producer: counter = %d", shared_counter);
        vTaskDelay(pdMS_TO_TICKS(1000)); // ทำงานทุก 1 วินาที
    }
}

void consumer_task(void *pvParameters)
{
    // Use TAG defined earlier or define a new one if needed
    // static const char *TAG_EX2 = "EXERCISE_2";
    ESP_LOGI(TAG, "Consumer Task started");
    int last_value = 0;

    while (1) {
        // ตรวจสอบว่าค่า counter เปลี่ยนแปลงหรือไม่
        if (shared_counter != last_value) {
            ESP_LOGI(TAG, "Consumer: received %d", shared_counter);
            last_value = shared_counter; // อัปเดตค่าล่าสุดที่อ่านได้
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // ตรวจสอบทุก 0.5 วินาที
    }
}
// --- END: Exercise 2 Code ---