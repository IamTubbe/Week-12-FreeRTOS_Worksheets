#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h" // <--- เพิ่มสำหรับ Semaphore (Challenge 1)
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h" // <--- เพิ่มสำหรับ High-res timer (Challenge 2)

static const char *TAG = "SW_TIMERS_ADV";

// LED pins
#define LED_BLINK GPIO_NUM_2
#define LED_HEARTBEAT GPIO_NUM_4
#define LED_STATUS GPIO_NUM_5
#define LED_ONESHOT GPIO_NUM_18

// Timer handles
TimerHandle_t xBlinkTimer;
TimerHandle_t xHeartbeatTimer;
TimerHandle_t xStatusTimer;
TimerHandle_t xOneShotTimer;
TimerHandle_t xDynamicTimer;
TimerHandle_t xExtraTimers[10];
TimerHandle_t xComplexSchedulerTimer; // <--- (Challenge 4)
TimerHandle_t xSyncTimerA;            // <--- (Challenge 1)
TimerHandle_t xSyncTimerB;            // <--- (Challenge 1)

// Handles for Synchronization
SemaphoreHandle_t xSyncSemaphore; // <--- (Challenge 1)

// Timer periods
#define BLINK_PERIOD 500
#define HEARTBEAT_PERIOD 2000
#define STATUS_PERIOD 5000
#define ONESHOT_DELAY 3000

// Statistics
typedef struct {
    uint32_t blink_count;
    uint32_t heartbeat_count;
    uint32_t status_count;
    uint32_t oneshot_count;
    uint32_t dynamic_count;
    uint32_t sync_a_count;
    uint32_t sync_b_count;
    uint32_t complex_count;
} timer_stats_t;

timer_stats_t stats = {0};

// LED states
bool led_blink_state = false;

// === Forward Declarations ===
void dynamic_timer_callback(TimerHandle_t xTimer);
void extra_callback(TimerHandle_t xTimer);
void complex_scheduler_callback(TimerHandle_t xTimer); // <--- (Challenge 4)
void sync_timer_A_callback(TimerHandle_t xTimer);      // <--- (Challenge 1)
void sync_timer_B_callback(TimerHandle_t xTimer);      // <--- (Challenge 1)
static void timer_sync_task(void *pvParameters);     // <--- (Challenge 1)

// Blink timer callback (auto-reload)
void blink_timer_callback(TimerHandle_t xTimer) {
    stats.blink_count++;
    
    // [CHALLENGE 2: Performance Analysis]
    // วัดความแม่นยำ (Jitter) ของไทม์เมอร์
    static uint64_t last_fire_time_us = 0;
    uint64_t now_us = esp_timer_get_time(); // <--- อ่านเวลาความละเอียดสูง (us)

    if (last_fire_time_us > 0) {
        uint32_t expected_period_ms = xTimerGetPeriod(xTimer) * portTICK_PERIOD_MS;
        int64_t jitter_us = (now_us - last_fire_time_us) - (expected_period_ms * 1000);
        
        // พิมพ์ Jitter (ความคลาดเคลื่อน)
        ESP_LOGI(TAG, "💫 Blink Timer: #%lu (Jitter: %lld us)", stats.blink_count, jitter_us);
    } else {
        ESP_LOGI(TAG, "💫 Blink Timer: #%lu (First run)", stats.blink_count);
    }
    last_fire_time_us = now_us;
    
    // Toggle LED
    led_blink_state = !led_blink_state;
    gpio_set_level(LED_BLINK, led_blink_state);
    
    // [CHALLENGE 5: Resource Management]
    // สาธิตการ "ใช้ซ้ำ" (Reuse) ไทม์เมอร์ที่มีอยู่
    if (stats.blink_count % 20 == 0) {
        ESP_LOGI(TAG, "🚀 Re-starting (reusing) one-shot timer.");
        if (xTimerStart(xOneShotTimer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Failed to re-start one-shot timer");
        }
    }
}

// Heartbeat timer callback (auto-reload)
void heartbeat_timer_callback(TimerHandle_t xTimer) {
    stats.heartbeat_count++;
    ESP_LOGI(TAG, "💓 Heartbeat Timer: Beat #%lu", stats.heartbeat_count);
    
    // Double blink
    gpio_set_level(LED_HEARTBEAT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);
}

// Status timer callback (auto-reload)
void status_timer_callback(TimerHandle_t xTimer) {
    stats.status_count++;
    ESP_LOGI(TAG, "📊 Status Timer: Update #%lu", stats.status_count);
    
    gpio_set_level(LED_STATUS, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_STATUS, 0);

    // [CHALLENGE 1: Timer Synchronization]
    // ส่ง Signal (Semaphore) ไปยัง Task ที่รอซิงค์ไทม์เมอร์
    if (xSyncSemaphore != NULL) {
        ESP_LOGI(TAG, "📊 Sending sync signal...");
        xSemaphoreGive(xSyncSemaphore);
    }
    
    ESP_LOGI(TAG, "═══ TIMER STATISTICS ═══");
    ESP_LOGI(TAG, "Blinks: %lu, Heartbeats: %lu, Status: %lu", 
             stats.blink_count, stats.heartbeat_count, stats.status_count);
    ESP_LOGI(TAG, "One-shot: %lu, Dynamic: %lu", 
             stats.oneshot_count, stats.dynamic_count);
    ESP_LOGI(TAG, "Sync A/B: %lu/%lu, Complex: %lu", 
             stats.sync_a_count, stats.sync_b_count, stats.complex_count);
    ESP_LOGI(TAG, "═══════════════════════");
}

// One-shot timer callback
void oneshot_timer_callback(TimerHandle_t xTimer) {
    stats.oneshot_count++;
    ESP_LOGI(TAG, "⚡ One-shot Timer: Event #%lu", stats.oneshot_count);
    
    for (int i = 0; i < 5; i++) {
        gpio_set_level(LED_ONESHOT, 1); vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_ONESHOT, 0); vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    uint32_t random_period = 1000 + (esp_random() % 3000);
    ESP_LOGI(TAG, "🎲 Creating dynamic timer (period: %lums)", random_period);
    
    xDynamicTimer = xTimerCreate("DynamicTimer",
                                 pdMS_TO_TICKS(random_period),
                                 pdFALSE, (void*)0,
                                 dynamic_timer_callback); 
    
    if (xDynamicTimer != NULL) {
        if (xTimerStart(xDynamicTimer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Failed to start dynamic timer");
        }
    } else {
        // [CHALLENGE 3: Error Handling]
        // ตรวจสอบความล้มเหลวในการสร้าง Timer (เช่น Heap หมด)
        ESP_LOGE(TAG, "Failed to create xDynamicTimer! Out of heap?");
    }
}

// Dynamic timer callback (created at runtime)
void dynamic_timer_callback(TimerHandle_t xTimer) {
    stats.dynamic_count++;
    ESP_LOGI(TAG, "🌟 Dynamic Timer: Event #%lu", stats.dynamic_count);
    
    // [CHALLENGE 5: Resource Management]
    // ไทม์เมอร์ที่สร้างแบบ dynamic (one-shot) ควรถูกลบ
    // เพื่อคืนหน่วยความจำ (Heap) กลับสู่ระบบ
    // (หมายเหตุ: One-shot timer มักจะถูกลบอัตโนมัติ, แต่การลบเองก็ชัดเจนดี)
    ESP_LOGI(TAG, "🌟 Deleting dynamic timer to free resources.");
    xTimerDelete(xTimer, pdMS_TO_TICKS(100));
    xDynamicTimer = NULL;
}

// Callback for the 10 extra timers
void extra_callback(TimerHandle_t xTimer) {
    uint32_t timer_id = (uint32_t)pvTimerGetTimerID(xTimer);
    ESP_LOGD(TAG, "🔔 Extra Timer #%lu fired!", timer_id); // ใช้ DEBUG log เพื่อไม่ให้รก
}

// [CHALLENGE 4: Complex Scheduling]
// Callback ที่ใช้ State Machine เพื่อเปลี่ยนคาบเวลาของตัวเอง
void complex_scheduler_callback(TimerHandle_t xTimer) {
    static int state = 0;
    stats.complex_count++;

    switch(state) {
        case 0:
            ESP_LOGI(TAG, "🗓️ Complex Sched: State 0 (running fast, 200ms). Next: slow.");
            // เปลี่ยนคาบเวลาถัดไปเป็น 1500ms
            xTimerChangePeriod(xTimer, pdMS_TO_TICKS(1500), 0);
            state = 1;
            break;
        case 1:
            ESP_LOGI(TAG, "🗓️ Complex Sched: State 1 (running slow, 1500ms). Next: medium.");
            // เปลี่ยนคาบเวลาถัดไปเป็น 800ms
            xTimerChangePeriod(xTimer, pdMS_TO_TICKS(800), 0);
            state = 2;
            break;
        case 2:
            ESP_LOGI(TAG, "🗓️ Complex Sched: State 2 (running medium, 800ms). Next: fast.");
            // เปลี่ยนคาบเวลาถัดไปเป็น 200ms (กลับไป state 0)
            xTimerChangePeriod(xTimer, pdMS_TO_TICKS(200), 0);
            state = 0;
            break;
    }
}

// [CHALLENGE 1: Timer Synchronization]
// Callbacks สำหรับไทม์เมอร์ที่จะถูกซิงค์
void sync_timer_A_callback(TimerHandle_t xTimer) {
    stats.sync_a_count++;
    ESP_LOGI(TAG, "SYNC 🅰️: Fired! (Count: %lu)", stats.sync_a_count);
}

void sync_timer_B_callback(TimerHandle_t xTimer) {
    stats.sync_b_count++;
    ESP_LOGI(TAG, "SYNC 🅱️: Fired! (Count: %lu)", stats.sync_b_count);
}

// [CHALLENGE 1: Timer Synchronization]
// Task ที่รอ Semaphore เพื่อเริ่มไทม์เมอร์ A และ B พร้อมกัน
static void timer_sync_task(void *pvParameters) {
    ESP_LOGI(TAG, "Sync Task started, waiting for signal...");
    while (1) {
        // รอสัญญาณจาก (ในที่นี้คือ status_timer_callback)
        if (xSemaphoreTake(xSyncSemaphore, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "SYNC TASK: Signal received! Starting timers A and B *now*.");
            
            // เริ่ม (หรือรีเซ็ต) ไทม์เมอร์ทั้งสองตัวในจังหวะที่ใกล้กันที่สุด
            // (ในบริบทของ Task เดียวกัน)
            xTimerReset(xSyncTimerA, pdMS_TO_TICKS(100));
            xTimerReset(xSyncTimerB, pdMS_TO_TICKS(100));
        }
    }
}

// Control task for timer management
void timer_control_task(void *pvParameters) {
    ESP_LOGI(TAG, "Timer control task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "\n🎛️  TIMER CONTROL: Performing maintenance...");
        int action = esp_random() % 3;
        
        // [CHALLENGE 3: Error Handling]
        // ตรวจสอบ pdFAIL จาก Timer Command Queue
        // (เช่น คิวเต็ม เพราะเรียกคำสั่งถี่ไป)
        switch (action) {
            case 0:
                ESP_LOGI(TAG, "⏸️  Stopping heartbeat timer");
                if (xTimerStop(xHeartbeatTimer, 100) != pdPASS) {
                    ESP_LOGW(TAG, "Cmd Queue full? Failed to stop timer.");
                }
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "▶️  Restarting heartbeat timer");
                if (xTimerStart(xHeartbeatTimer, 100) != pdPASS) {
                    ESP_LOGW(TAG, "Cmd Queue full? Failed to start timer.");
                }
                break;
                
            case 1:
                ESP_LOGI(TAG, "🔄 Reset status timer");
                if (xTimerReset(xStatusTimer, 100) != pdPASS) {
                    ESP_LOGW(TAG, "Cmd Queue full? Failed to reset timer.");
                }
                break;
                
            case 2:
                ESP_LOGI(TAG, "⚙️  Changing blink timer period");
                uint32_t new_period = 200 + (esp_random() % 600);
                if (xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 100) != pdPASS) {
                    ESP_LOGW(TAG, "Cmd Queue full? Failed to change period.");
                }
                break;
        }
        ESP_LOGI(TAG, "Maintenance completed\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Software Timers Advanced Lab Starting...");
    
    // Config LEDs
    gpio_set_direction(LED_BLINK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_HEARTBEAT, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_ONESHOT, GPIO_MODE_OUTPUT);
    
    // [CHALLENGE 1: Timer Synchronization]
    // สร้าง Semaphore เพื่อใช้เป็นตัวส่งสัญญาณ
    vSemaphoreCreateBinary(xSyncSemaphore);
    if (xSyncSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create xSyncSemaphore!");
    }
    
    ESP_LOGI(TAG, "Creating software timers...");
    
    // --- Create Timers ---
    xBlinkTimer = xTimerCreate("BlinkTimer", pdMS_TO_TICKS(BLINK_PERIOD), pdTRUE, (void*)1, blink_timer_callback);
    xHeartbeatTimer = xTimerCreate("HeartbeatTimer", pdMS_TO_TICKS(HEARTBEAT_PERIOD), pdTRUE, (void*)2, heartbeat_timer_callback);
    xStatusTimer = xTimerCreate("StatusTimer", pdMS_TO_TICKS(STATUS_PERIOD), pdTRUE, (void*)3, status_timer_callback);
    xOneShotTimer = xTimerCreate("OneShotTimer", pdMS_TO_TICKS(ONESHOT_DELAY), pdFALSE, (void*)4, oneshot_timer_callback);

    // [CHALLENGE 4: Complex Scheduling]
    xComplexSchedulerTimer = xTimerCreate("ComplexSched", pdMS_TO_TICKS(200), pdTRUE, (void*)5, complex_scheduler_callback);

    // [CHALLENGE 1: Timer Synchronization]
    // สร้างไทม์เมอร์ A และ B (เป็น one-shot, จะถูก reset โดย sync_task)
    xSyncTimerA = xTimerCreate("SyncTimerA", pdMS_TO_TICKS(3000), pdFALSE, (void*)6, sync_timer_A_callback);
    xSyncTimerB = xTimerCreate("SyncTimerB", pdMS_TO_TICKS(3000), pdFALSE, (void*)7, sync_timer_B_callback);

    // [CHALLENGE 3: Error Handling]
    // ตรวจสอบการสร้างไทม์เมอร์หลักอย่างชัดเจน
    if (xBlinkTimer == NULL || xHeartbeatTimer == NULL || xStatusTimer == NULL || 
        xOneShotTimer == NULL || xComplexSchedulerTimer == NULL || 
        xSyncTimerA == NULL || xSyncTimerB == NULL) 
    {
        ESP_LOGE(TAG, "Failed to create one or more base timers! Out of heap?");
        return; // หยุดทำงาน
    }

    ESP_LOGI(TAG, "All base timers created successfully");
        
    // --- Start Timers ---
    ESP_LOGI(TAG, "Starting timers...");
    xTimerStart(xBlinkTimer, 0);
    xTimerStart(xHeartbeatTimer, 0);
    xTimerStart(xStatusTimer, 0);
    xTimerStart(xComplexSchedulerTimer, 0);
    // (OneShotTimer, Sync Timers, และ Dynamic Timers จะถูกเริ่มในภายหลัง)
        
    // --- Create Tasks ---
    xTaskCreate(timer_control_task, "TimerControl", 2048, NULL, 5, NULL);
    xTaskCreate(timer_sync_task, "TimerSync", 2048, NULL, 5, NULL); // <--- (Challenge 1)
        
    // --- Create 10 Extra Timers ---
    ESP_LOGI(TAG, "Creating 10 extra auto-reload timers...");
    char timer_name[20];
    for (int i = 0; i < 10; i++) {
        sprintf(timer_name, "ExtraTimer%d", i); 
        xExtraTimers[i] = xTimerCreate(timer_name, 
                                        pdMS_TO_TICKS(1000 + i * 200), // Period 1s - 3s
                                        pdTRUE, (void*)i, extra_callback);
        
        if (xExtraTimers[i]) {
            xTimerStart(xExtraTimers[i], 0);
        } else {
            ESP_LOGE(TAG, "Failed to create ExtraTimer #%d", i);
            break; // หยุดถ้า Heap หมด
        }
    }

    ESP_LOGI(TAG, "Timer system operational!");
}
