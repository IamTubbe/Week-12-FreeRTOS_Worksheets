#include <stdio.h>
#include <stdlib.h> // จำเป็นสำหรับ malloc/free
#include <string.h> // จำเป็นสำหรับ memset
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h" // จำเป็นสำหรับ esp_get_free_heap_size etc.

// --- Function Prototypes ---
// ประกาศฟังก์ชันทั้งหมดที่จะใช้ เพื่อให้คอมไพเลอร์รู้จักก่อนเรียกใช้
void state_demo_task(void *pvParameters);
void ready_state_demo_task(void *pvParameters);
void control_task(void *pvParameters);
void system_monitor_task(void *pvParameters);
void self_deleting_task(void *pvParameters); // Step 2 Advanced
void external_delete_task(void *pvParameters); // Step 2 Advanced
void monitor_task_states(void); // Step 3
const char* get_state_name(eTaskState state); // Helper
void count_state_change(eTaskState old_state, eTaskState new_state); // Exercise 1
void update_state_display(eTaskState current_state); // Exercise 2

// --- Defines ---
// กำหนดขา GPIO สำหรับ LED แสดงสถานะต่างๆ
#define LED_RUNNING GPIO_NUM_2      // แสดง Running state
#define LED_READY GPIO_NUM_4        // แสดง Ready state
#define LED_BLOCKED GPIO_NUM_5      // แสดง Blocked state
#define LED_SUSPENDED GPIO_NUM_18   // แสดง Suspended state

// กำหนดขา GPIO สำหรับปุ่มกด
#define BUTTON1_PIN GPIO_NUM_0      // ปุ่มควบคุม (Suspend/Resume)
#define BUTTON2_PIN GPIO_NUM_35     // ปุ่มเปลี่ยนสถานะ (Give Semaphore)

// Tag สำหรับการแสดง Log
static const char *TAG = "TASK_STATES";

// --- Global Variables ---
// Handles สำหรับอ้างอิงถึง Task ที่สร้างขึ้น
TaskHandle_t state_demo_task_handle = NULL;
TaskHandle_t control_task_handle = NULL;
TaskHandle_t external_delete_handle = NULL; // Handle สำหรับ Task ที่จะถูกลบจากภายนอก

// Semaphore สำหรับสาธิตสถานะ Blocked
SemaphoreHandle_t demo_semaphore = NULL; // ประกาศครั้งเดียวตรงนี้

// --- Exercise 1: State Transition Counter ---
volatile uint32_t state_changes[5] = {0}; // Array สำหรับนับจำนวนครั้งที่เข้าสู่แต่ละสถานะ (0=Run, 1=Ready, 2=Block, 3=Suspend, 4=Delete)
// ------------------------------------------

// ชื่อสถานะต่างๆ สำหรับแสดงผล
const char* state_names[] = {
    "Running",      // eRunning = 0
    "Ready",        // eReady = 1
    "Blocked",      // eBlocked = 2
    "Suspended",    // eSuspended = 3
    "Deleted",      // eDeleted = 4
    "Invalid"       // eInvalid = 5
};

// --- Helper Function ---
// ฟังก์ชันสำหรับแปลงค่า eTaskState เป็นข้อความชื่อสถานะ
const char* get_state_name(eTaskState state)
{
    // ใช้ค่า enum เป็น index โดยตรง พร้อมตรวจสอบขอบเขต
    if (state >= eRunning && state <= eInvalid) {
        return state_names[state];
    }
    return "Unknown"; // กรณีค่า state ไม่ถูกต้อง
}

// --- Task Definitions ---

// Task หลักสำหรับสาธิตสถานะต่างๆ (นำการควบคุม LED ออก)
void state_demo_task(void *pvParameters)
{
    ESP_LOGI(TAG, "State Demo Task started (Priority %d)", uxTaskPriorityGet(NULL));
    int cycle = 0;

    while (1) {
        cycle++;
        ESP_LOGI(TAG, "=== StateDemo Cycle %d ===", cycle);

        // --- สถานะ: Running ---
        ESP_LOGI(TAG, "StateDemo: RUNNING (doing work)");
        // (LED จะถูกควบคุมโดย update_state_display ใน control_task)

        // จำลองการทำงานหนักเพื่อให้อยู่ในสถานะ Running นานขึ้น
        for (volatile int i = 0; i < 1000000; i++) {} // ใช้ volatile ป้องกันการ optimization

        // --- สถานะ: Ready ---
        ESP_LOGI(TAG, "StateDemo: Yielding -> READY");
        // (LED จะถูกควบคุมโดย update_state_display ใน control_task)

        taskYIELD(); // เปิดโอกาสให้ Task อื่น (ReadyDemo) ที่มี Priority เท่ากันทำงาน
        // หลังจาก yield กลับมา อาจจะ Running ต่อทันที หรือถ้ามี Task อื่นทำงานอยู่ ก็จะ Ready
        vTaskDelay(pdMS_TO_TICKS(10)); // การ Delay สั้นๆ ทำให้เข้า Blocked -> Ready ก่อนจะ Running ใหม่

        // --- สถานะ: Blocked (รอ Semaphore) ---
        ESP_LOGI(TAG, "StateDemo: Waiting for semaphore -> BLOCKED");
        // (LED จะถูกควบคุมโดย update_state_display ใน control_task)

        // รอ Semaphore พร้อม Timeout 5 วินาที
        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
            ESP_LOGI(TAG, "StateDemo: Got semaphore! -> RUNNING");
            // (LED จะถูกควบคุมโดย update_state_display ใน control_task)
            // จำลองการทำงานหลังได้ Semaphore (ซึ่งจะทำให้ Blocked อีกครั้ง!)
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            ESP_LOGW(TAG, "StateDemo: Semaphore timeout! -> READY/RUNNING");
            // (LED จะถูกควบคุมโดย update_state_display ใน control_task)
             vTaskDelay(pdMS_TO_TICKS(100)); // Delay สั้นๆ
        }

        // --- สถานะ: Blocked (รอ vTaskDelay) ---
        ESP_LOGI(TAG, "StateDemo: Entering vTaskDelay -> BLOCKED");
        // (LED จะถูกควบคุมโดย update_state_display ใน control_task)

        vTaskDelay(pdMS_TO_TICKS(1500)); // Task จะ Blocked อยู่ที่นี่

        // (LED จะถูกควบคุมโดย update_state_display ใน control_task เมื่อ Delay จบ)
        ESP_LOGD(TAG, "StateDemo: End of cycle, will loop."); // Log สำหรับ Debug
    }
}

// Task ที่มี Priority เท่ากับ state_demo_task เพื่อสาธิตสถานะ Ready
void ready_state_demo_task(void *pvParameters)
{
     ESP_LOGI(TAG, "Ready Demo Task started (Priority %d)", uxTaskPriorityGet(NULL));
    while (1) {
        // Task นี้จะได้ทำงานเมื่อ state_demo_task อยู่ในสถานะ Blocked หรือ Yield
        ESP_LOGI(TAG, "ReadyDemo: RUNNING");

        // จำลองการทำงาน
        for (volatile int i = 0; i < 500000; i++) {}

        vTaskDelay(pdMS_TO_TICKS(200)); // Block ตัวเอง
    }
}

// Control task (รวม Exercise 1, Exercise 2, Step 3)
void control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Control Task started (Priority %d)", uxTaskPriorityGet(NULL));

    // --- ตัวแปรสำหรับ Control Task ---
    bool is_suspended = false; // ติดตามสถานะ Suspend/Resume
    int control_cycle = 0;     // ตัวนับรอบการทำงาน
    bool external_deleted = false; // ติดตามว่า Task สำหรับ Step 2 ถูกลบไปหรือยัง
    eTaskState previous_demo_state = eInvalid; // *** ประกาศตัวแปรเก็บ state ก่อนหน้า ***

    while (1) {
        control_cycle++; // เพิ่มตัวนับรอบ

        // --- ตรวจสอบปุ่มกด 1 (GPIO 0) - สั่ง Suspend/Resume ---
        if (gpio_get_level(BUTTON1_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
            if (gpio_get_level(BUTTON1_PIN) == 0) { // ตรวจสอบอีกครั้ง
                if (state_demo_task_handle != NULL) { // ตรวจสอบ Handle
                    if (!is_suspended) {
                        // สั่ง Suspend
                        ESP_LOGW(TAG, "CONTROL: Button 1 -> SUSPENDING StateDemo Task");
                        vTaskSuspend(state_demo_task_handle);
                        is_suspended = true;
                        update_state_display(eSuspended); // *** Ex2: อัปเดต LED ทันที ***
                        previous_demo_state = eSuspended; // อัปเดต state ที่ติดตาม
                    } else {
                        // สั่ง Resume
                        ESP_LOGW(TAG, "CONTROL: Button 1 -> RESUMING StateDemo Task");
                        vTaskResume(state_demo_task_handle);
                        is_suspended = false;
                        // LED จะอัปเดตในการตรวจสอบสถานะรอบถัดไป
                    }
                } else { ESP_LOGE(TAG, "CONTROL: StateDemo Task handle is NULL!"); }
                // รอปล่อยปุ่ม
                while (gpio_get_level(BUTTON1_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        // --- ตรวจสอบปุ่มกด 2 (GPIO 35) - สั่ง Give Semaphore ---
        if (gpio_get_level(BUTTON2_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
             if (gpio_get_level(BUTTON2_PIN) == 0) { // ตรวจสอบอีกครั้ง
                ESP_LOGW(TAG, "CONTROL: Button 2 -> GIVING Semaphore");
                if (demo_semaphore != NULL) { xSemaphoreGive(demo_semaphore); }
                else { ESP_LOGE(TAG, "CONTROL: Demo semaphore is NULL!"); }
                // รอปล่อยปุ่ม
                while (gpio_get_level(BUTTON2_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        // --- Step 2 Advanced: ลบ external_delete_task หลังจากเวลาผ่านไป ---
        if (control_cycle == 150 && !external_deleted && external_delete_handle != NULL) { // ประมาณ 15 วินาที
            ESP_LOGW(TAG, "CONTROL: Deleting ExternalDelete Task");
            vTaskDelete(external_delete_handle);
            external_delete_handle = NULL; // ตั้ง Handle เป็น NULL
            external_deleted = true;      // ตั้ง Flag
        }

        // --- เรียกใช้ Step 3: แสดงสถานะ Task อย่างละเอียด ทุกๆ 6 วินาที ---
        if (control_cycle % 60 == 0) { // 60 * 100ms = 6 วินาที
             monitor_task_states(); // <-- เรียกใช้ฟังก์ชัน Step 3
        }

        // --- แสดงสถานะ Task แบบย่อ, นับ State Change (Ex1), อัปเดต LED (Ex2) ทุกๆ 3 วินาที ---
        if (control_cycle % 30 == 0) { // 30 * 100ms = 3 วินาที
            ESP_LOGI(TAG, "--- TASK STATUS REPORT (Basic) & LED Update ---");
            eTaskState current_demo_state = eInvalid; // ค่าเริ่มต้น
            if (state_demo_task_handle != NULL) {
                current_demo_state = eTaskGetState(state_demo_task_handle); // อ่านสถานะปัจจุบัน
                if (current_demo_state != eDeleted && current_demo_state != eInvalid) {
                    ESP_LOGI(TAG, "StateDemo: State=%s", get_state_name(current_demo_state));

                    // --- Exercise 1: State Change ---
                    if (previous_demo_state != eInvalid) { // ข้ามการเช็คครั้งแรก
                        count_state_change(previous_demo_state, current_demo_state);
                    }
                } else {
                    ESP_LOGW(TAG, "StateDemo: Task might be deleted or invalid.");
                    // เรียก count_state_change ด้วยถ้า state เปลี่ยนเป็น Deleted/Invalid
                    if (previous_demo_state != eInvalid && previous_demo_state != current_demo_state) {
                         count_state_change(previous_demo_state, current_demo_state);
                    }
                }
            } else {
                ESP_LOGW(TAG, "StateDemo: Handle is NULL (Task might be deleted).");
                current_demo_state = eDeleted; // สมมติว่าถูกลบ
                 // เรียก count_state_change ถ้า state ก่อนหน้าไม่ใช่ Deleted
                if (previous_demo_state != eDeleted) {
                     count_state_change(previous_demo_state, eDeleted);
                }
            }

             // --- Exercise 2: Update LED Display ---
             // เรียกใช้เสมอ เพื่ออัปเดต LED ตามสถานะล่าสุด (รวมถึง Deleted/Invalid)
             update_state_display(current_demo_state);

             // อัปเดต previous_demo_state *หลัง* จากใช้เปรียบเทียบและอัปเดต LED แล้ว
             previous_demo_state = current_demo_state;
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // หน่วงเวลา 100ms ก่อนเริ่มรอบถัดไป
    }
}

// Task สำหรับแสดงสถิติระบบ (ต้องการการตั้งค่าใน menuconfig)
void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System Monitor Task started (Priority %d)", uxTaskPriorityGet(NULL));
    char *task_list_buffer = malloc(2048); // เพิ่มขนาด Buffer
    char *stats_buffer = malloc(2048);     // เพิ่มขนาด Buffer
    if (!task_list_buffer || !stats_buffer) { ESP_LOGE(TAG, "Monitor: Failed to allocate buffers!"); vTaskDelete(NULL); return; }

    while (1) {
        ESP_LOGI(TAG, "\n--- SYSTEM MONITOR ---");
        // --- Task List ---
        memset(task_list_buffer, 0, 2048); // ล้าง Buffer ก่อนใช้
        vTaskList(task_list_buffer);
        printf("--- Task List ---\nName\t\tState\tPrio\tStackRem\t#\n%s-------------------\n", task_list_buffer);
        // --- Runtime Statistics ---
        memset(stats_buffer, 0, 2048); // ล้าง Buffer ก่อนใช้
        vTaskGetRunTimeStats(stats_buffer);
        printf("--- Runtime Stats ---\nTask\t\tAbs Time\t%%Time\n%s---------------------\n", stats_buffer);
        vTaskDelay(pdMS_TO_TICKS(10000)); // รายงานทุก 10 วินาที
    }
    free(task_list_buffer); free(stats_buffer); // ไม่ควรมาถึงตรงนี้
}

// --- Step 2 Advanced: Task Definitions ---
// Task ที่สาธิตการลบตัวเอง
void self_deleting_task(void *pvParameters)
{
    int lifetime_sec = *((int *)pvParameters);
    ESP_LOGI(TAG, "SelfDelete Task started (Lifetime: %d s)", lifetime_sec);
    for (int i = lifetime_sec; i > 0; i--) {
        ESP_LOGI(TAG, "SelfDelete Task: Countdown %d s", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "SelfDelete Task: Self-deleting now -> DELETED");
    vTaskDelete(NULL); // ลบตัวเอง
}

// Task ที่จะถูกลบจากภายนอก
void external_delete_task(void *pvParameters)
{
    int count = 0;
    ESP_LOGI(TAG, "ExternalDelete Task started");
    while (1) {
        ESP_LOGI(TAG, "ExternalDelete Task: Running cycle %d", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- START: Step 3 Code ---
// ฟังก์ชันสำหรับแสดงสถานะ Task อย่างละเอียด
void monitor_task_states(void)
{
    ESP_LOGI(TAG, "=== DETAILED TASK STATE MONITOR ===");
    TaskHandle_t tasks[] = { state_demo_task_handle, control_task_handle }; // รายการ Task ที่จะตรวจสอบ
    const char* task_names[] = { "StateDemo", "Control" };
    int num_tasks = sizeof(tasks) / sizeof(tasks[0]);

    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i] != NULL) {
            eTaskState state = eTaskGetState(tasks[i]);
            if (state != eDeleted && state != eInvalid) {
                UBaseType_t priority = uxTaskPriorityGet(tasks[i]);
                UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(tasks[i]);
                ESP_LOGI(TAG, "%s: State=%s, Priority=%d, StackRem=%lu bytes",
                         task_names[i], get_state_name(state), priority, stack_remaining * sizeof(StackType_t));
            } else { ESP_LOGW(TAG, "%s: Task state is %s", task_names[i], get_state_name(state)); }
        } else { ESP_LOGW(TAG, "Task handle at index %d is NULL", i); }
    }
    ESP_LOGI(TAG, "=================================");
}
// --- END: Step 3 Code ---

// --- START: Exercise 1 Code ---
// ฟังก์ชันนับจำนวนครั้งที่ Task เปลี่ยนสถานะ
void count_state_change(eTaskState old_state, eTaskState new_state)
{
    if (old_state != new_state && new_state <= eDeleted) {
        state_changes[new_state]++;
        ESP_LOGI(TAG, "STATE CHANGE DETECTED: %s -> %s (Count for %s: %lu)",
                 get_state_name(old_state), get_state_name(new_state),
                 get_state_name(new_state), state_changes[new_state]);
    }
}
// --- END: Exercise 1 Code ---

// --- START: Exercise 2 Code ---
// ฟังก์ชันอัปเดต LED ตามสถานะ Task
void update_state_display(eTaskState current_state)
{
    // ปิด LED ทั้งหมดก่อน
    gpio_set_level(LED_RUNNING, 0);
    gpio_set_level(LED_READY, 0);
    gpio_set_level(LED_BLOCKED, 0);
    gpio_set_level(LED_SUSPENDED, 0);

    // เปิด LED ที่ตรงกับสถานะปัจจุบัน
    switch (current_state) {
        case eRunning:   gpio_set_level(LED_RUNNING, 1);   break;
        case eReady:     gpio_set_level(LED_READY, 1);     break;
        case eBlocked:   gpio_set_level(LED_BLOCKED, 1);   break;
        case eSuspended: gpio_set_level(LED_SUSPENDED, 1); break;
        default: // eDeleted, eInvalid หรือสถานะที่ไม่รู้จัก
            // กะพริบ LED ทั้งหมด 2 ครั้ง
            for (int i = 0; i < 2; i++) {
                gpio_set_level(LED_RUNNING, 1); gpio_set_level(LED_READY, 1); gpio_set_level(LED_BLOCKED, 1); gpio_set_level(LED_SUSPENDED, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(LED_RUNNING, 0); gpio_set_level(LED_READY, 0); gpio_set_level(LED_BLOCKED, 0); gpio_set_level(LED_SUSPENDED, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            break;
    }
}
// --- END: Exercise 2 Code ---

// --- Main Function ---
void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Task States Demo ===");

    // --- GPIO Configuration ---
    gpio_config_t io_conf_led = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_RUNNING) | (1ULL << LED_READY) | (1ULL << LED_BLOCKED) | (1ULL << LED_SUSPENDED),
        .pull_down_en = 0, .pull_up_en = 0,
    };
    gpio_config(&io_conf_led);
    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON1_PIN) | (1ULL << BUTTON2_PIN),
        .pull_up_en = 1, .pull_down_en = 0,
    };
    gpio_config(&button_conf);

    // --- Create Semaphore ---
    demo_semaphore = xSemaphoreCreateBinary();
    if (demo_semaphore == NULL) { ESP_LOGE(TAG, "Failed to create demo_semaphore!"); return; }
    ESP_LOGI(TAG, "Demo semaphore created.");

    // --- Log Info ---
    ESP_LOGI(TAG, "LED Indicators:"); /* ... Log messages ... */
    ESP_LOGI(TAG, "Button Controls:"); /* ... Log messages ... */

    // --- Create Tasks ---
    ESP_LOGI(TAG, "Creating tasks...");
    BaseType_t res_state = xTaskCreate(state_demo_task, "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    BaseType_t res_ready = xTaskCreate(ready_state_demo_task, "ReadyDemo", 2048, NULL, 3, NULL);
    BaseType_t res_ctrl = xTaskCreate(control_task, "Control", 3072, NULL, 4, &control_task_handle);
    BaseType_t res_mon = xTaskCreate(system_monitor_task, "Monitor", 4096, NULL, 1, NULL);
    static int self_delete_time_sec = 10;
    BaseType_t res_selfdel = xTaskCreate(self_deleting_task, "SelfDelete", 2048, &self_delete_time_sec, 2, NULL);
    BaseType_t res_extdel = xTaskCreate(external_delete_task, "ExtDelete", 2048, NULL, 2, &external_delete_handle);

    // Check task creation results
    if (res_state != pdPASS || res_ready != pdPASS || res_ctrl != pdPASS || res_mon != pdPASS || res_selfdel != pdPASS || res_extdel != pdPASS) {
        ESP_LOGE(TAG, "Failed to create one or more tasks!");
    } else {
        ESP_LOGI(TAG, "All required tasks created successfully.");
    }

    ESP_LOGI(TAG, "Monitoring task states...");
    // app_main จบการทำงานที่นี่ แต่ Task อื่นๆ ยังคงทำงานต่อไป
}