#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

// --- ADD FUNCTION PROTOTYPES HERE ---
void high_priority_task(void *pvParameters);
void medium_priority_task(void *pvParameters);
void low_priority_task(void *pvParameters);
void control_task(void *pvParameters);
// Add these lines:
void equal_priority_task1(void *pvParameters);
void equal_priority_task2(void *pvParameters);
void equal_priority_task3(void *pvParameters);
// --- End of Prototypes ---

// Add these lines for Step 3:
void priority_inversion_high(void *pvParameters);
void priority_inversion_low(void *pvParameters);
// --- End of Prototypes ---

void dynamic_priority_demo(void *pvParameters); // <-- เพิ่มบรรทัดนี้

#define LED_HIGH_PIN GPIO_NUM_2    // สำหรับ High Priority Task
#define LED_MED_PIN GPIO_NUM_4     // สำหรับ Medium Priority Task
#define LED_LOW_PIN GPIO_NUM_5     // สำหรับ Low Priority Task
#define BUTTON_PIN GPIO_NUM_0      // Trigger button

static const char *TAG = "PRIORITY_DEMO";

// Global variables for statistics
volatile uint32_t high_task_count = 0;
volatile uint32_t med_task_count = 0;
volatile uint32_t low_task_count = 0;
volatile bool priority_test_running = false;

// High Priority Task (Priority 5) - Pinned to Core 0
void high_priority_task(void *pvParameters)
{
    // แสดง Core ID ตอนเริ่ม Task
    ESP_LOGI(TAG, "High Priority Task started (Priority 5) on Core %d", xPortGetCoreID());

    while (1) {
        if (priority_test_running) {
            high_task_count++;
            // แสดง Core ID ทุกครั้งที่ทำงาน
            ESP_LOGI(TAG, "HIGH PRIORITY RUNNING on Core %d (%lu)", xPortGetCoreID(), high_task_count);

            gpio_set_level(LED_HIGH_PIN, 1);
            // Simulate critical work
            for (int i = 0; i < 100000; i++) {
                volatile int dummy = i * 2;
            }
            gpio_set_level(LED_HIGH_PIN, 0);

            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// Medium Priority Task (Priority 3) - No Affinity (Runs on Any Core)
void medium_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Priority Task started (Priority 3) on Core %d", xPortGetCoreID());

    while (1) {
        if (priority_test_running) {
            med_task_count++;
            // แสดง Core ID ทุกครั้งที่ทำงาน
            ESP_LOGI(TAG, "Medium priority running on Core %d (%lu)", xPortGetCoreID(), med_task_count);

            gpio_set_level(LED_MED_PIN, 1);
            // Simulate moderate work
            for (int i = 0; i < 200000; i++) {
                volatile int dummy = i + 100;
            }
            gpio_set_level(LED_MED_PIN, 0);

            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// Low Priority Task (Priority 1) - Pinned to Core 1
void low_priority_task(void *pvParameters)
{
    // แสดง Core ID ตอนเริ่ม Task
    ESP_LOGI(TAG, "Low Priority Task started (Priority 1) on Core %d", xPortGetCoreID());

    while (1) {
        if (priority_test_running) {
            low_task_count++;
            // แสดง Core ID ทุกครั้งที่ทำงาน
            ESP_LOGI(TAG, "Low priority running on Core %d (%lu)", xPortGetCoreID(), low_task_count);

            gpio_set_level(LED_LOW_PIN, 1);
            // Simulate background work
            for (int i = 0; i < 500000; i++) {
                volatile int dummy = i - 50;
                if (i % 100000 == 0) {
                    vTaskDelay(1); // Yield
                }
            }
            gpio_set_level(LED_LOW_PIN, 0);

            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}


// Control Task - starts/stops the test
void control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Control Task started on Core %d", xPortGetCoreID());

    while (1) {
        if (gpio_get_level(BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
            if (gpio_get_level(BUTTON_PIN) == 0) {
                if (!priority_test_running) {
                    ESP_LOGW(TAG, "=== STARTING PRIORITY TEST ===");
                    high_task_count = 0;
                    med_task_count = 0;
                    low_task_count = 0;
                    priority_test_running = true;

                    ESP_LOGI(TAG, "Waiting for button release to start test...");
                    while(gpio_get_level(BUTTON_PIN) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    ESP_LOGI(TAG, "Button released, starting 10s test.");

                    vTaskDelay(pdMS_TO_TICKS(10000)); // Run test for 10 seconds

                    priority_test_running = false;
                    ESP_LOGW(TAG, "=== PRIORITY TEST STOPPED ===");
                    ESP_LOGW(TAG, "=== PRIORITY TEST RESULTS ===");
                    ESP_LOGI(TAG, "High Priority Task (Core 0) runs: %lu", high_task_count);
                    ESP_LOGI(TAG, "Medium Priority Task (Any Core) runs: %lu", med_task_count);
                    ESP_LOGI(TAG, "Low Priority Task (Core 1) runs: %lu", low_task_count);

                    uint32_t total_runs = high_task_count + med_task_count + low_task_count;
                    if (total_runs > 0) {
                        ESP_LOGI(TAG, "High priority percentage: %.1f%%", (float)high_task_count / total_runs * 100);
                        ESP_LOGI(TAG, "Medium priority percentage: %.1f%%", (float)med_task_count / total_runs * 100);
                        ESP_LOGI(TAG, "Low priority percentage: %.1f%%", (float)low_task_count / total_runs * 100);
                    }
                } else {
                     // If already running, wait for release to avoid double trigger
                     while(gpio_get_level(BUTTON_PIN) == 0) {
                         vTaskDelay(pdMS_TO_TICKS(50));
                     }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Check button every 100ms
    }
}


// เพิ่มใน main file

// Tasks ที่มี priority เท่ากัน
void equal_priority_task1(void *pvParameters)
{
    int task_id = 1;
    
    while (1) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "Equal Priority Task %d running", task_id);
            
            // Simulate work without yielding
            for (int i = 0; i < 300000; i++) {
                volatile int dummy = i;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50)); // Short delay
    }
}

void equal_priority_task2(void *pvParameters)
{
    int task_id = 2;
    
    while (1) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "Equal Priority Task %d running", task_id);
            
            // Simulate work without yielding
            for (int i = 0; i < 300000; i++) {
                volatile int dummy = i;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50)); // Short delay
    }
}

void equal_priority_task3(void *pvParameters)
{
    int task_id = 3;
    
    while (1) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "Equal Priority Task %d running", task_id);
            
            // Simulate work without yielding
            for (int i = 0; i < 300000; i++) {
                volatile int dummy = i;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50)); // Short delay
    }
}

// Exercise 1: เปลี่ยน Priority แบบ Dynamic
void dynamic_priority_demo(void *pvParameters)
{
    // ต้องแน่ใจว่าได้รับ handle ที่ถูกต้อง! ดูตัวอย่างใน app_main ด้านล่าง
    TaskHandle_t task_to_control = (TaskHandle_t)pvParameters;

    // เพิ่มการตรวจสอบ handle เพื่อความปลอดภัย
    if (task_to_control == NULL) {
         ESP_LOGE(TAG, "Dynamic Priority Demo received NULL handle!");
         vTaskDelete(NULL); // ออกจาก Task ถ้า handle ไม่ถูกต้อง
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // รอ 5 วินาที

        ESP_LOGW(TAG, "Boosting task priority to 4");
        // เปลี่ยน Priority ของ Task ที่รับมาเป็น 4
        vTaskPrioritySet(task_to_control, 4);

        vTaskDelay(pdMS_TO_TICKS(2000)); // รอ 2 วินาที

        ESP_LOGW(TAG, "Restoring task priority to 1");
        // เปลี่ยน Priority กลับเป็น 1
        vTaskPrioritySet(task_to_control, 1);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Priority Scheduling Demo ===");
    
    // GPIO Configuration
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_HIGH_PIN) | (1ULL << LED_MED_PIN) | (1ULL << LED_LOW_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // Button configuration
    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .pull_up_en = 1,
        .pull_down_en = 0,
    };
    gpio_config(&button_conf);

    ESP_LOGI(TAG, "Creating tasks with different priorities...");

    // --- Pin tasks to specific cores (Exercise 2) ---
    // Pin high_priority_task to Core 0
    BaseType_t res_high = xTaskCreatePinnedToCore(
        high_priority_task, "HighPrio", 3072, NULL, 5, NULL, 0);
    if(res_high == pdPASS) ESP_LOGI(TAG, "Pinned High Priority Task to Core 0"); else ESP_LOGE(TAG, "Failed to pin High Prio Task");

    // Pin low_priority_task to Core 1
    BaseType_t res_low = xTaskCreatePinnedToCore(
        low_priority_task, "LowPrio", 3072, NULL, 1, NULL, 1);
    if(res_low == pdPASS) ESP_LOGI(TAG, "Pinned Low Priority Task to Core 1"); else ESP_LOGE(TAG, "Failed to pin Low Prio Task");

    // --- Create other tasks without pinning ---
    // Create medium priority task (no affinity specified, FreeRTOS decides)
    BaseType_t res_med = xTaskCreate(medium_priority_task, "MedPrio", 3072, NULL, 3, NULL);
    if(res_med == pdPASS) ESP_LOGI(TAG, "Created Medium Priority Task (No Affinity)"); else ESP_LOGE(TAG, "Failed to create Med Prio Task");

    // Create control task (no affinity specified)
    BaseType_t res_ctrl = xTaskCreate(control_task, "Control", 3072, NULL, 4, NULL);
    if(res_ctrl == pdPASS) ESP_LOGI(TAG, "Created Control Task (No Affinity)"); else ESP_LOGE(TAG, "Failed to create Control Task");
    
    // --- ประกาศ Handles สำหรับเก็บ Task ที่สร้าง ---
    TaskHandle_t high_task_handle = NULL;
    TaskHandle_t med_task_handle = NULL;
    TaskHandle_t low_task_handle_for_demo = NULL; // *** ตัวแปรนี้สำคัญสำหรับ Exercise 1 ***
    TaskHandle_t control_task_handle = NULL;
    TaskHandle_t dynamic_demo_handle = NULL;
    
    // Create tasks with different priorities
    xTaskCreate(high_priority_task, "HighPrio", 3072, NULL, 5, &high_task_handle);
    xTaskCreate(medium_priority_task, "MedPrio", 3072, NULL, 3, &med_task_handle);
    xTaskCreate(low_priority_task, "LowPrio", 3072, NULL, 1, &low_task_handle_for_demo);
    xTaskCreate(control_task, "Control", 3072, NULL, 4, NULL);
    xTaskCreate(equal_priority_task1, "Equal1", 2048, NULL, 2, NULL);
    xTaskCreate(equal_priority_task2, "Equal2", 2048, NULL, 2, NULL);
    xTaskCreate(equal_priority_task3, "Equal3", 2048, NULL, 2, NULL);

    // สร้าง Task ที่ Priority ต่างกันเพื่อสาธิต
    ESP_LOGI(TAG, "Creating Priority Inversion Demo Tasks...");
    xTaskCreate(priority_inversion_high, "InvHigh", 2048, NULL, 5, NULL); // Priority สูง
    xTaskCreate(medium_priority_task, "MedPrio", 3072, NULL, 3, NULL); // Task Priority กลาง (เพื่อมาขวาง)
    xTaskCreate(priority_inversion_low, "InvLow", 2048, NULL, 1, NULL); // Priority ต่ำ (แต่จะถือ Resource)
    xTaskCreate(control_task, "Control", 3072, NULL, 4, NULL); // Control task ยังจำเป็นอยู่

    // --- สร้าง Task สำหรับ Exercise 1 ---
    ESP_LOGI(TAG, "Creating Dynamic Priority Demo Task...");
    // *** ตรวจสอบว่าได้ handle มาจริงก่อนสร้าง Task ***
    if (low_task_handle_for_demo != NULL) {
         // *** ส่ง handle ของ Low Priority Task (low_task_handle_for_demo) เป็นพารามิเตอร์ ***
         xTaskCreate(dynamic_priority_demo, "DynamicDemo", 2048, (void*)low_task_handle_for_demo, 4, &dynamic_demo_handle); // กำหนด Priority ให้เหมาะสม (เช่น 4)
    } else {
         ESP_LOGE(TAG, "Cannot create Dynamic Demo Task: Low priority task handle is NULL");
    }
    
    ESP_LOGI(TAG, "Press button to start priority test");
    ESP_LOGI(TAG, "Watch LEDs: GPIO2=High (Core 0), GPIO5=Low (Core 1), GPIO4=Med (Any Core)");
}

// เพิ่มการสาธิต Priority Inversion (ปัญหาที่อาจเกิดขึ้น)

volatile bool shared_resource_busy = false;

void priority_inversion_high(void *pvParameters)
{
    while (1) {
        if (priority_test_running) {
            ESP_LOGW(TAG, "High priority task needs shared resource");
            
            // รอ shared resource
            while (shared_resource_busy) {
                ESP_LOGW(TAG, "High priority BLOCKED by low priority!");
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            
            ESP_LOGI(TAG, "High priority task got resource");
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void priority_inversion_low(void *pvParameters)
{
    while (1) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "Low priority task using shared resource");
            shared_resource_busy = true;
            
            // จำลองการใช้ resource นาน
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            shared_resource_busy = false;
            ESP_LOGI(TAG, "Low priority task released resource");
        }
        
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}