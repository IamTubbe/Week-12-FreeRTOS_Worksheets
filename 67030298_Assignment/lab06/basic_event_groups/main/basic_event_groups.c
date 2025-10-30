#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"      // C1, C3
#include "freertos/semphr.h"     // C2
#include "esp_log.h"
#include "esp_random.h"
#include "driver/gpio.h"

static const char *TAG = "EVENT_GROUPS_ADV";

// ... (GPIO Defines, Event Bits, Stats Struct - เหมือนเดิม) ...
#define LED_NETWORK_READY   GPIO_NUM_2
#define LED_SENSOR_READY    GPIO_NUM_4  
#define LED_CONFIG_READY    GPIO_NUM_5 
#define LED_STORAGE_READY   GPIO_NUM_18 
#define LED_SYSTEM_READY    GPIO_NUM_19 

EventGroupHandle_t system_events;
#define NETWORK_READY_BIT   (1 << 0)
#define SENSOR_READY_BIT    (1 << 1)
#define CONFIG_READY_BIT    (1 << 2)
#define STORAGE_READY_BIT   (1 << 3)
#define SYSTEM_READY_BIT    (1 << 4)

#define BASIC_SYSTEM_BITS   (NETWORK_READY_BIT | CONFIG_READY_BIT)
#define ALL_SUBSYSTEM_BITS  (NETWORK_READY_BIT | SENSOR_READY_BIT | \
                             CONFIG_READY_BIT | STORAGE_READY_BIT)
#define FULL_SYSTEM_BITS    (ALL_SUBSYSTEM_BITS | SYSTEM_READY_BIT)

typedef struct {
    uint32_t network_init_time;
    uint32_t sensor_init_time;
    uint32_t config_init_time; 
    uint32_t storage_init_time;
    uint32_t total_init_time;
    uint32_t event_notifications;
} system_stats_t;

static system_stats_t stats = {0};

// ================ C1: Priority Events ================
typedef enum {
    ALERT_NETWORK_DOWN,
    ALERT_SENSOR_FAIL,
    ALERT_CONFIG_CORRUPT
} AlertID_t;

typedef struct {
    AlertID_t id;
    int priority; // 0=Low, 1=Medium, 2=High
    TickType_t timestamp;
} SystemAlertMsg_t;

QueueHandle_t alert_queue;
// =====================================================

// ================ C2: Event Logging ================
#define EVENT_LOG_SIZE 50
typedef struct {
    TickType_t timestamp;
    char context[20];
    uint32_t value;
} EventLogEntry_t;

EventLogEntry_t event_log[EVENT_LOG_SIZE];
uint32_t event_log_index = 0;
SemaphoreHandle_t log_mutex;
// ===================================================

// ================ C3/C5: Dynamic Events & Optimization ================
#define CLEANUP_DONE_BIT (1 << 0) // Bit ที่จะใช้ใน Task Notification

typedef struct {
    TaskHandle_t requester_handle; // C5: Handle ของ Task ที่ร้องขอ
} StorageCmd_t;

QueueHandle_t storage_cmd_queue;
// ====================================================================

// --- (ฟังก์ชัน debug_event_bits และ print_event_statistics เหมือนเดิม) ---
void debug_event_bits(EventBits_t bits, const char* context) {
    ESP_LOGI(TAG, "🐛 DEBUG %s - Event bits: 0x%08X", context, bits);
    ESP_LOGI(TAG, "  Network: %s", (bits & NETWORK_READY_BIT) ? "SET" : "CLEAR");
    ESP_LOGI(TAG, "  Sensor:  %s", (bits & SENSOR_READY_BIT) ? "SET" : "CLEAR");
    ESP_LOGI(TAG, "  Config:  %s", (bits & CONFIG_READY_BIT) ? "SET" : "CLEAR");
    ESP_LOGI(TAG, "  Storage: %s", (bits & STORAGE_READY_BIT) ? "SET" : "CLEAR");
    ESP_LOGI(TAG, "  System:  %s", (bits & SYSTEM_READY_BIT) ? "SET" : "CLEAR");
}

void print_event_statistics(void) {
    uint32_t uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    float event_rate = 0.0;
    if (uptime_ms > 0) {
        event_rate = (float)stats.event_notifications * 60000.0 / uptime_ms;
    }
    ESP_LOGI(TAG, "\n📈 EVENT STATISTICS");
    ESP_LOGI(TAG, "Total notifications: %lu", stats.event_notifications);
    ESP_LOGI(TAG, "System uptime: %lu ms", uptime_ms);
    ESP_LOGI(TAG, "Event rate: %.2f events/min", event_rate);
}

// ================ C2: Event Logging Function ================
/**
 * @brief บันทึก Event ลงใน Circular Buffer (Thread-safe)
 */
void add_to_event_log(const char* context, uint32_t value) {
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        event_log[event_log_index].timestamp = xTaskGetTickCount();
        strncpy(event_log[event_log_index].context, context, 19);
        event_log[event_log_index].value = value;
        event_log_index = (event_log_index + 1) % EVENT_LOG_SIZE;
        xSemaphoreGive(log_mutex);
    }
}
// ============================================================

// ================ C1: Priority Event Handler Task ================
/**
 * @brief Task (Priority สูง) ที่รอรับ Alert ที่สำคัญ
 */
void alert_handler_task(void *pvParameters) {
    SystemAlertMsg_t received_alert;
    ESP_LOGI(TAG, "[C1] Alert Handler Task started (Priority 10)");

    while (1) {
        if (xQueueReceive(alert_queue, &received_alert, portMAX_DELAY) == pdTRUE) {
            
            stats.event_notifications++;
            add_to_event_log("Alert Received", received_alert.id); // C2

            switch(received_alert.id) {
                case ALERT_NETWORK_DOWN:
                    ESP_LOGE(TAG, "[C1] HIGH PRIORITY ALERT: Network is DOWN! (Prio: %d)", received_alert.priority);
                    // TODO: ดำเนินการฉุกเฉิน เช่น reboot หรือเข้า Safe Mode
                    break;
                case ALERT_SENSOR_FAIL:
                    ESP_LOGW(TAG, "[C1] MEDIUM PRIORITY ALERT: Sensor failure! (Prio: %d)", received_alert.priority);
                    break;
                case ALERT_CONFIG_CORRUPT:
                     ESP_LOGW(TAG, "[C1] MEDIUM PRIORITY ALERT: Config corrupt! (Prio: %d)", received_alert.priority);
                    break;
            }
        }
    }
}
// ===================================================================

// ================ C4: Event Correlation Task ================
/**
 * @brief Task (Priority ต่ำ) ที่วิเคราะห์ Log เพื่อหาความสัมพันธ์
 */
void event_correlation_task(void *pvParameters) {
    ESP_LOGI(TAG, "[C4] Event Correlation Task started (Priority 2)");
    
    while(1) {
        // วิเคราะห์ทุก 30 วินาที
        vTaskDelay(pdMS_TO_TICKS(30000)); 

        if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            TickType_t network_fail_time = 0;
            TickType_t sensor_fail_time = 0;

            // สแกน Log (C2)
            for (int i = 0; i < EVENT_LOG_SIZE; i++) {
                if (strcmp(event_log[i].context, "Network Failure") == 0) {
                    network_fail_time = event_log[i].timestamp;
                }
                if (strcmp(event_log[i].context, "Sensor Failure") == 0) {
                    sensor_fail_time = event_log[i].timestamp;
                }

                // C4 Logic: ถ้า Sensor ล่มภายใน 5 วินาทีหลังจาก Network ล่ม
                if (network_fail_time > 0 && sensor_fail_time > network_fail_time) {
                    if ((sensor_fail_time - network_fail_time) < pdMS_TO_TICKS(5000)) {
                        ESP_LOGW(TAG, "[C4] CORRELATION DETECTED: Sensor failure occurred shortly after Network failure!");
                        // Reset เพื่อไม่ให้ báo cáo ซ้ำ
                        network_fail_time = 0; 
                        sensor_fail_time = 0;
                    }
                }
            }
            xSemaphoreGive(log_mutex);
        }
    }
}
// ============================================================


// --- Network initialization task (Modified for C1, C2) ---
void network_init_task(void *pvParameters) {
    ESP_LOGI(TAG, "🌐 Network initialization started");
    uint32_t start_time = xTaskGetTickCount();
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(5000));
    vTaskDelay(pdMS_TO_TICKS(6000));
    
    stats.network_init_time = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
    gpio_set_level(LED_NETWORK_READY, 1);
    xEventGroupSetBits(system_events, NETWORK_READY_BIT);
    stats.event_notifications++;
    add_to_event_log("NetworkInit End", NETWORK_READY_BIT); // C2
    ESP_LOGI(TAG, "✅ Network ready! (took %lu ms)", stats.network_init_time);
    
    debug_event_bits(xEventGroupGetBits(system_events), "NetworkInit End");
    
    while (1) {
        if ((esp_random() % 100) > 5) {
             gpio_set_level(LED_NETWORK_READY, 1);
             if (!(xEventGroupGetBits(system_events) & NETWORK_READY_BIT)) {
                 xEventGroupSetBits(system_events, NETWORK_READY_BIT);
                 ESP_LOGI(TAG, "🟢 Network connection restored");
                 stats.event_notifications++;
                 add_to_event_log("Network Restored", 1); // C2
             }
         } else {
             gpio_set_level(LED_NETWORK_READY, 0);
             xEventGroupClearBits(system_events, NETWORK_READY_BIT);
             ESP_LOGW(TAG, "🔴 Network connection lost");
             debug_event_bits(xEventGroupGetBits(system_events), "Network Failure");
             add_to_event_log("Network Failure", 0); // C2
             
             // C1: ส่ง Alert ที่มี Priority สูง
             SystemAlertMsg_t msg = {ALERT_NETWORK_DOWN, 2, xTaskGetTickCount()};
             xQueueSend(alert_queue, &msg, 0); 
         }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// --- Sensor initialization task (Modified for C1, C2) ---
void sensor_init_task(void *pvParameters) {
    ESP_LOGI(TAG, "🌡️ Sensor initialization started");
    uint32_t start_time = xTaskGetTickCount();
    
    vTaskDelay(pdMS_TO_TICKS(200)); 
    vTaskDelay(pdMS_TO_TICKS(800)); 
    vTaskDelay(pdMS_TO_TICKS(500)); 
    vTaskDelay(pdMS_TO_TICKS(500)); 
    
    stats.sensor_init_time = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
    gpio_set_level(LED_SENSOR_READY, 1);
    xEventGroupSetBits(system_events, SENSOR_READY_BIT);
    ESP_LOGI(TAG, "✅ Sensors ready! (took %lu ms)", stats.sensor_init_time);
    stats.event_notifications++;
    add_to_event_log("SensorInit End", SENSOR_READY_BIT); // C2
    
    debug_event_bits(xEventGroupGetBits(system_events), "SensorInit End");
    
    while (1) {
        float temperature = 25.0 + (esp_random() % 200) / 10.0;
        float humidity = 40.0 + (esp_random() % 400) / 10.0; 
        
        ESP_LOGI(TAG, "🌡️ Sensor readings: %.1f°C, %.1f%% RH", temperature, humidity);
        
        if (temperature > 50.0 || humidity > 90.0) {
            ESP_LOGW(TAG, "⚠️ Sensor values out of range!");
            gpio_set_level(LED_SENSOR_READY, 0);
            xEventGroupClearBits(system_events, SENSOR_READY_BIT);
            add_to_event_log("Sensor Failure", 0); // C2

            // C1: ส่ง Alert ที่มี Priority ปานกลาง
            SystemAlertMsg_t msg = {ALERT_SENSOR_FAIL, 1, xTaskGetTickCount()};
            xQueueSend(alert_queue, &msg, 0);

            vTaskDelay(pdMS_TO_TICKS(2000));
            gpio_set_level(LED_SENSOR_READY, 1);
            xEventGroupSetBits(system_events, SENSOR_READY_BIT);
            ESP_LOGI(TAG, "🟢 Sensor system recovered");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// Configuration loading task (Modified for C2)
void config_load_task(void *pvParameters) {
    ESP_LOGI(TAG, "⚙️ Configuration loading started");
    uint32_t start_time = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(600));
    vTaskDelay(pdMS_TO_TICKS(400));
    vTaskDelay(pdMS_TO_TICKS(300));
    vTaskDelay(pdMS_TO_TICKS(500));
    
    stats.config_init_time = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
    gpio_set_level(LED_CONFIG_READY, 1);
    xEventGroupSetBits(system_events, CONFIG_READY_BIT);
    ESP_LOGI(TAG, "✅ Configuration loaded! (took %lu ms)", stats.config_init_time);
    stats.event_notifications++;
    add_to_event_log("ConfigInit End", CONFIG_READY_BIT); // C2
    
    debug_event_bits(xEventGroupGetBits(system_events), "ConfigInit End");
    
    while (1) {
        if ((esp_random() % 100) > 2) {
             gpio_set_level(LED_CONFIG_READY, 1);
           } else {
             ESP_LOGW(TAG, "⚠️ Configuration corruption detected, reloading...");
             gpio_set_level(LED_CONFIG_READY, 0);
             xEventGroupClearBits(system_events, CONFIG_READY_BIT);
             add_to_event_log("Config Corrupt", 0); // C2

             // C1: ส่ง Alert
             SystemAlertMsg_t msg = {ALERT_CONFIG_CORRUPT, 1, xTaskGetTickCount()};
             xQueueSend(alert_queue, &msg, 0);

             vTaskDelay(pdMS_TO_TICKS(1000));
             gpio_set_level(LED_CONFIG_READY, 1);
             xEventGroupSetBits(system_events, CONFIG_READY_BIT);
             ESP_LOGI(TAG, "🟢 Configuration reloaded successfully");
           }
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}

// Storage initialization task (Modified for C2, C3, C5)
void storage_init_task(void *pvParameters) {
    ESP_LOGI(TAG, "💾 Storage initialization started");
    uint32_t start_time = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(1500));
    vTaskDelay(pdMS_TO_TICKS(300));
    vTaskDelay(pdMS_TO_TICKS(800));
    
    stats.storage_init_time = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
    gpio_set_level(LED_STORAGE_READY, 1);
    xEventGroupSetBits(system_events, STORAGE_READY_BIT);
    ESP_LOGI(TAG, "✅ Storage ready! (took %lu ms)", stats.storage_init_time);
    stats.event_notifications++;
    add_to_event_log("StorageInit End", STORAGE_READY_BIT); // C2
    
    debug_event_bits(xEventGroupGetBits(system_events), "StorageInit End");
    
    StorageCmd_t received_cmd; // C3

    while (1) {
        // C3: รอรับคำสั่ง Dynamic Event (เช่น Cleanup) หรือ Timeout เพื่อทำงานปกติ
        if (xQueueReceive(storage_cmd_queue, &received_cmd, pdMS_TO_TICKS(10000)) == pdTRUE) {
            
            // C3: ได้รับคำสั่ง Dynamic Event
            ESP_LOGI(TAG, "[C3] Dynamic Event Received: Starting Cache Cleanup...");
            add_to_event_log("Dynamic Cleanup", 0); // C2

            // จำลองการทำงานหนัก (Cleanup)
            vTaskDelay(pdMS_TO_TICKS(2000)); 

            ESP_LOGI(TAG, "[C3] Cache Cleanup Complete.");
            
            // C5: Optimization - แจ้ง Task ที่ร้องขอกลับโดยตรง
            if (received_cmd.requester_handle != NULL) {
                xTaskNotify(received_cmd.requester_handle, CLEANUP_DONE_BIT, eSetBits);
            }

        } else {
            // C3: Timeout (ไม่มีคำสั่ง) - ทำงาน Maintenance ปกติ
            uint32_t free_space = 1000 + (esp_random() % 9000);
            ESP_LOGI(TAG, "💾 Storage maintenance - free space: %lu MB", free_space);
            if (free_space < 500) {
                ESP_LOGW(TAG, "⚠️ Low storage space warning!");
            }
        }
    }
}

// Main system coordinator task (Modified for C2, C3, C5)
void system_coordinator_task(void *pvParameters) {
    ESP_LOGI(TAG, "🎛️ System coordinator started - waiting for subsystems...");
    
    uint32_t total_start_time = xTaskGetTickCount();
    uint32_t wait_start_time; 
    EventBits_t bits;
    uint32_t wait_duration;
    
    // --- Phase 1: Timing Analysis ---
    ESP_LOGI(TAG, "📋 Phase 1: Waiting for basic subsystems (Network + Config)... Timeout: 4000ms");
    wait_start_time = xTaskGetTickCount(); 
    bits = xEventGroupWaitBits(system_events, BASIC_SYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(4000));
    wait_duration = (xTaskGetTickCount() - wait_start_time) * portTICK_PERIOD_MS;
    
    if ((bits & BASIC_SYSTEM_BITS) == BASIC_SYSTEM_BITS) {
        ESP_LOGI(TAG, "✅ Phase 1 complete! Took %lu ms.", wait_duration);
        stats.event_notifications++;
        add_to_event_log("Phase 1 OK", bits); // C2
    } else {
        EventBits_t missing_bits = BASIC_SYSTEM_BITS & ~bits;
        ESP_LOGW(TAG, "❌❌ Phase 1 TIMEOUT! (Waited %lu ms)", wait_duration);
        ESP_LOGW(TAG, "  Status: Network=%s, Config=%s",
                 (missing_bits & NETWORK_READY_BIT) ? "❌" : "✅",
                 (missing_bits & CONFIG_READY_BIT) ? "❌" : "✅");
        debug_event_bits(bits, "Phase 1 Timeout");
        add_to_event_log("Phase 1 Timeout", missing_bits); // C2
    }
    
    // --- Phase 2: Timing Analysis ---
    ESP_LOGI(TAG, "📋 Phase 2: Waiting for all subsystems... Timeout: 6000ms");
    wait_start_time = xTaskGetTickCount(); 
    bits = xEventGroupWaitBits(system_events, ALL_SUBSYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(6000));
    wait_duration = (xTaskGetTickCount() - wait_start_time) * portTICK_PERIOD_MS;
    
    if ((bits & ALL_SUBSYSTEM_BITS) == ALL_SUBSYSTEM_BITS) {
        ESP_LOGI(TAG, "✅ Phase 2 complete! Took %lu ms.", wait_duration);
        
        xEventGroupSetBits(system_events, SYSTEM_READY_BIT);
        gpio_set_level(LED_SYSTEM_READY, 1);
        
        stats.total_init_time = (xTaskGetTickCount() - total_start_time) * portTICK_PERIOD_MS;
        stats.event_notifications++;
        add_to_event_log("Phase 2 OK", bits); // C2
        
        ESP_LOGI(TAG, "🎉 SYSTEM FULLY OPERATIONAL! 🎉");
        ESP_LOGI(TAG, "═══ INITIALIZATION COMPLETE ═══");
        ESP_LOGI(TAG, "Total init time: %lu ms", stats.total_init_time);
        ESP_LOGI(TAG, "Network init: ... %lu ms", stats.network_init_time);
        ESP_LOGI(TAG, "Sensor init: ... %lu ms", stats.sensor_init_time);
        ESP_LOGI(TAG, "Config init: ... %lu ms", stats.config_init_time);
        ESP_LOGI(TAG, "Storage init: .. %lu ms", stats.storage_init_time);
        ESP_LOGI(TAG, "══════════════════════════════════");
        
        print_event_statistics();
        
    } else {
        EventBits_t missing_bits = ALL_SUBSYSTEM_BITS & ~bits;
        ESP_LOGW(TAG, "❌❌ Phase 2 TIMEOUT! (Waited %lu ms)", wait_duration);
        ESP_LOGW(TAG, "  Status: Network=%s, Sensor=%s, Config=%s, Storage=%s",
                 (missing_bits & NETWORK_READY_BIT) ? "❌" : "✅",
                 (missing_bits & SENSOR_READY_BIT) ? "❌" : "✅",
                 (missing_bits & CONFIG_READY_BIT) ? "❌" : "✅",
                 (missing_bits & STORAGE_READY_BIT) ? "❌" : "✅");
        debug_event_bits(bits, "Phase 2 Timeout");
        add_to_event_log("Phase 2 Timeout", missing_bits); // C2
        ESP_LOGW(TAG, "Starting with limited functionality...");
    }
    
    // --- Phase 3: System monitoring & C3/C5 ---
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); // ทำงานทุก 15 วินาที

        // C3: Dynamic Event Request
        ESP_LOGI(TAG, "[C3] Requesting dynamic storage cleanup...");
        StorageCmd_t cmd = {
            .requester_handle = xTaskGetCurrentTaskHandle() // C5: ส่ง Handle ของตัวเอง
        };
        xQueueSend(storage_cmd_queue, &cmd, 0);

        // C5: Optimization - รอการแจ้งเตือน (Notify) แทน Event Group
        uint32_t notification_value = 0;
        BaseType_t notify_result = xTaskNotifyWait(
            0x00,               // ไม่ Clear bits ใดๆ ตอนเริ่ม
            ULONG_MAX,          // Clear ทุก bits เมื่อได้รับ
            &notification_value, // ค่าที่ได้รับ
            pdMS_TO_TICKS(5000) // Timeout 5 วินาที
        );

        if (notify_result == pdTRUE && (notification_value & CLEANUP_DONE_BIT)) {
            ESP_LOGI(TAG, "[C5] Dynamic cleanup complete (Confirmed by TaskNotify)");
            add_to_event_log("Cleanup OK", notification_value); // C2
        } else {
            ESP_LOGE(TAG, "[C5] Dynamic cleanup FAILED (Timeout or wrong bit)");
            add_to_event_log("Cleanup Fail", notification_value); // C2
        }

        // ส่วน Health Check เดิม (ย้ายมาทำงานหลัง C3/C5)
        EventBits_t current_bits = xEventGroupGetBits(system_events);
        ESP_LOGI(TAG, "🔄 System health check: 0x%08X", current_bits);
        if ((current_bits & ALL_SUBSYSTEM_BITS) != ALL_SUBSYSTEM_BITS) {
             ESP_LOGW(TAG, "⚠️ System degraded - some subsystems offline");
             gpio_set_level(LED_SYSTEM_READY, 0);
             xEventGroupClearBits(system_events, SYSTEM_READY_BIT);
           } else if (!(current_bits & SYSTEM_READY_BIT)) {
             ESP_LOGI(TAG, "🟢 All subsystems back online - system ready");
             gpio_set_level(LED_SYSTEM_READY, 1);
             xEventGroupSetBits(system_events, SYSTEM_READY_BIT);
           }
    }
}

// Event monitor task
void event_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "👁️ Event monitor started");
    while (1) {
        ESP_LOGI(TAG, "🔍 Monitoring events...");
        
        debug_event_bits(xEventGroupGetBits(system_events), "Monitor Before Wait");

        ESP_LOGI(TAG, "Waiting for ANY subsystem event (5 second timeout)...");
        EventBits_t bits = xEventGroupWaitBits(
            system_events, ALL_SUBSYSTEM_BITS, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000)
        );
        
        if (bits != 0) {
            ESP_LOGI(TAG, "📢 Event detected: 0x%08X", bits);
            if (bits & NETWORK_READY_BIT) ESP_LOGI(TAG, "   🌐 Network event active");
            if (bits & SENSOR_READY_BIT)  ESP_LOGI(TAG, "   🌡️ Sensor event active");
            if (bits & CONFIG_READY_BIT)  ESP_LOGI(TAG, "   ⚙️ Config event active");
            if (bits & STORAGE_READY_BIT) ESP_LOGI(TAG, "   💾 Storage event active");
            stats.event_notifications++; 
        } else {
            ESP_LOGI(TAG, "⏰ No events within timeout period");
        }
        
        if (!(xEventGroupGetBits(system_events) & SYSTEM_READY_BIT)) {
            // ... (Full system ready logic) ...
        }
        
        print_event_statistics();
        
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "🚀 Advanced Event Groups Lab Starting (All Challenges)...");
    
    // --- Setup GPIOs ---
    gpio_set_direction(LED_NETWORK_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SENSOR_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONFIG_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STORAGE_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM_READY, GPIO_MODE_OUTPUT);
    
    gpio_set_level(LED_NETWORK_READY, 0);
    gpio_set_level(LED_SENSOR_READY, 0);
    gpio_set_level(LED_CONFIG_READY, 0);
    gpio_set_level(LED_STORAGE_READY, 0);
    gpio_set_level(LED_SYSTEM_READY, 0);
    
    // --- Create RTOS Objects ---
    system_events = xEventGroupCreate();
    
    // C1: Create Priority Alert Queue
    alert_queue = xQueueCreate(10, sizeof(SystemAlertMsg_t));
    
    // C2: Create Log Mutex
    log_mutex = xSemaphoreCreateMutex();

    // C3/C5: Create Storage Command Queue
    storage_cmd_queue = xQueueCreate(5, sizeof(StorageCmd_t));

    if (system_events == NULL || alert_queue == NULL || log_mutex == NULL || storage_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RTOS objects!");
        return;
    }
    
    ESP_LOGI(TAG, "All RTOS objects created successfully");
    add_to_event_log("AppMain Start", 0); // C2
    
    // --- Create Tasks ---
    xTaskCreate(network_init_task, "NetworkInit", 3072, NULL, 6, NULL);
    xTaskCreate(sensor_init_task, "SensorInit", 2048, NULL, 5, NULL);
    xTaskCreate(config_load_task, "ConfigLoad", 2048, NULL, 4, NULL);
    xTaskCreate(storage_init_task, "StorageInit", 3072, NULL, 4, NULL); // เพิ่ม Stack ให้ Storage Task
    
    xTaskCreate(system_coordinator_task, "SysCoord", 3072, NULL, 8, NULL);
    xTaskCreate(event_monitor_task, "EventMon", 2048, NULL, 3, NULL);

    // C1: Create Alert Handler Task (Priority 10)
    xTaskCreate(alert_handler_task, "AlertHandler", 2048, NULL, 10, NULL);

    // C4: Create Correlation Task (Priority 2)
    xTaskCreate(event_correlation_task, "EventCorrelation", 3072, NULL, 2, NULL);
    
    ESP_LOGI(TAG, "All tasks created successfully");
    ESP_LOGI(TAG, "Advanced Event Groups system operational!");
}
