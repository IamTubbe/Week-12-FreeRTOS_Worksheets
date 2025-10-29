// main/time_sharing.c - โค้ดทดลองแบบวนลูป Time Slice อัตโนมัติ
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

// --- กำหนดขา LED ---
#define LED1_PIN GPIO_NUM_2 // Task Sensor (งานเบา)
#define LED2_PIN GPIO_NUM_4 // Task Process (งานหนัก)
#define LED3_PIN GPIO_NUM_5 // Task Actuator (งานปานกลาง)
#define LED4_PIN GPIO_NUM_18 // Task Display (งานเบา)

static const char *TAG = "TIME_SHARING_EXPERIMENT";

// --- กำหนด Task ID ---
typedef enum {
    TASK_SENSOR,
    TASK_PROCESS,
    TASK_ACTUATOR,
    TASK_DISPLAY,
    TASK_COUNT 
} task_id_t;

// --- Time slice (ms) ---
// **************************************************************************
// *** เราจะลบ #define ค่าคงที่ทิ้ง และใช้ตัวแปรใน app_main แทน ***
// #define TIME_SLICE_MS 200 
// **************************************************************************

// --- ตัวแปรสำหรับเก็บสถิติ (ใช้สำหรับแต่ละรอบการทดสอบ) ---
static volatile uint32_t task_counter = 0; 
static volatile uint64_t task_execution_time_total = 0; 
static volatile uint32_t context_switches = 0; 
static volatile uint64_t scheduler_overhead_total = 0; 

// --- ฟังก์ชันจำลองการทำงานของ Task ต่างๆ (Workload ปรับจูนใหม่) ---
// เป้าหมาย: เพิ่มความหนักของงาน (Workload) ให้มากขึ้น

void simulate_sensor_task(void)
{
    // ESP_LOGI(TAG, "[LED1 ON ] Starting Sensor Task (Light Work)..."); // (A)
    gpio_set_level(LED1_PIN, 1);
    uint64_t start = esp_timer_get_time();
    // ปรับจาก 10,000 -> 4,300,000 (เพิ่ม ~430x)
    for (long i = 0; i < 4300000; i++) { volatile int dummy = i; } 
    uint64_t duration = esp_timer_get_time() - start;
    gpio_set_level(LED1_PIN, 0);
    // ESP_LOGI(TAG, "[LED1 OFF] Sensor Task finished (%llu us).", duration); // (B)
}

void simulate_processing_task(void)
{
    // ESP_LOGI(TAG, "[LED2 ON ] Starting Processing Task (Heavy Work)..."); // (A)
    gpio_set_level(LED2_PIN, 1);
    uint64_t start = esp_timer_get_time();
    // ปรับจาก 100,000 -> 4,300,000 (เพิ่ม ~43x)
    for (long i = 0; i < 4300000; i++) { volatile int dummy = i * i; } 
    uint64_t duration = esp_timer_get_time() - start;
    gpio_set_level(LED2_PIN, 0);
    // ESP_LOGI(TAG, "[LED2 OFF] Processing Task finished (%llu us).", duration); // (B)
}

void simulate_actuator_task(void)
{
    // ESP_LOGI(TAG, "[LED3 ON ] Starting Actuator Task (Medium Work)..."); // (A)
    gpio_set_level(LED3_PIN, 1);
    uint64_t start = esp_timer_get_time();
    // ปรับจาก 50,000 -> 2,150,000 (เพิ่ม ~43x)
    for (long i = 0; i < 2150000; i++) { volatile int dummy = i + 100; } 
    uint64_t duration = esp_timer_get_time() - start;
    gpio_set_level(LED3_PIN, 0);
    // ESP_LOGI(TAG, "[LED3 OFF] Actuator Task finished (%llu us).", duration); // (B)
}

void simulate_display_task(void)
{
    // ESP_LOGI(TAG, "[LED4 ON ] Starting Display Task (Light Work)..."); // (A)
    gpio_set_level(LED4_PIN, 1);
    uint64_t start = esp_timer_get_time();
    // ปรับจาก 20,000 -> 860,000 (เพิ่ม ~43x)
    for (long i = 0; i < 860000; i++) { volatile int dummy = i / 2; } 
    uint64_t duration = esp_timer_get_time() - start;
    gpio_set_level(LED4_PIN, 0);
    // ESP_LOGI(TAG, "[LED4 OFF] Display Task finished (%llu us).", duration); // (B)
}

// --- Scheduler แบบ Manual (Overhead ปรับจูนใหม่) ---
void manual_scheduler(void)
{
    uint64_t scheduler_start_time = esp_timer_get_time();

    // Overhead ส่วนที่ 1 (ปรับจาก 1,000 -> 150,000) (เพิ่ม 150x)
    context_switches++; // นับสวิตช์ที่นี่
    for (int i = 0; i < 150000; i++) { volatile int dummy = i; }

    uint64_t task_start_time = esp_timer_get_time();

    // เลือกรัน Task
    switch (task_counter % TASK_COUNT) {
        case TASK_SENSOR:   simulate_sensor_task();   break;
        case TASK_PROCESS:  simulate_processing_task(); break;
        case TASK_ACTUATOR: simulate_actuator_task(); break;
        case TASK_DISPLAY:  simulate_display_task();  break;
    }

    uint64_t task_end_time = esp_timer_get_time();
    task_execution_time_total += (task_end_time - task_start_time);

    // Overhead ส่วนที่ 2 (ปรับจาก 1,000 -> 150,000) (เพิ่ม 150x)
    for (int i = 0; i < 150000; i++) { volatile int dummy = i; }

    uint64_t scheduler_end_time = esp_timer_get_time();
    scheduler_overhead_total += (scheduler_end_time - scheduler_start_time) - (task_end_time - task_start_time);

    task_counter++;
}

// --- Main Function (ปรับปรุงใหม่) ---
void app_main(void)
{
    // ตั้งค่า GPIO สำหรับ LED
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) |
                        (1ULL << LED3_PIN) | (1ULL << LED4_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Time-Sharing System Started (Original Workload)");
    ESP_LOGI(TAG, "Starting variable time slice experiment...");

    // --- ค่า Time Slices ที่ต้องการทดสอบ (ms) ---
    uint32_t time_slices_to_test[] = {10, 25, 50, 100, 200}; // ค่าจากโจทย์และโค้ดเดิม
    int num_slices = sizeof(time_slices_to_test) / sizeof(time_slices_to_test[0]);
    
    // --- ระยะเวลาทดสอบสำหรับแต่ละ Time Slice (วินาที) ---
    const int TEST_DURATION_SEC = 10; // ทดสอบนาน 10 วินาที เพื่อให้ได้ค่าเฉลี่ยที่นิ่ง
    const uint64_t TEST_DURATION_US = TEST_DURATION_SEC * 1000000ULL;

    while(1) { // วนลูปการทดลองทั้งหมด
        
        for (int i = 0; i < num_slices; i++) {
            uint32_t current_time_slice_ms = time_slices_to_test[i];
            
            ESP_LOGI(TAG, "--------------------------------------------------");
            ESP_LOGI(TAG, "Testing Time Slice: %u ms (for %d seconds)", current_time_slice_ms, TEST_DURATION_SEC);

            // --- Reset สถิติสำหรับรอบการทดสอบใหม่ ---
            task_counter = 0;
            task_execution_time_total = 0;
            context_switches = 0;
            scheduler_overhead_total = 0;

            uint64_t loop_start_time = esp_timer_get_time();
            uint64_t current_loop_time = loop_start_time;

            // --- รันการทดสอบตามระยะเวลาที่กำหนด ---
            while ( (current_loop_time = esp_timer_get_time()) - loop_start_time < TEST_DURATION_US) 
            {
                manual_scheduler(); // รัน Task 1 ครั้ง (นับ 1 context switch)

                // รอตามเวลา Time Slice ที่กำหนด
                vTaskDelay(pdMS_TO_TICKS(current_time_slice_ms));
            }

            // --- สิ้นสุดการทดสอบ: คำนวณและแสดงผล ---
            uint64_t total_elapsed_time = current_loop_time - loop_start_time;
            
            if (total_elapsed_time > 0 && context_switches > 0) {
                float total_elapsed_sec = (float)total_elapsed_time / 1000000.0f;
                
                // คำนวณค่าต่างๆ
                float context_switches_per_sec = (float)context_switches / total_elapsed_sec;
                float cpu_utilization = ((float)task_execution_time_total / (float)total_elapsed_time) * 100.0f;
                float overhead_percentage = ((float)scheduler_overhead_total / (float)total_elapsed_time) * 100.0f;
                uint64_t avg_task_time_us = task_execution_time_total / context_switches;
                uint64_t avg_overhead_time_us = scheduler_overhead_total / context_switches;
                
                // คำนวณเวลา Idle (เวลาที่ใช้ใน vTaskDelay)
                uint64_t total_work_time = task_execution_time_total + scheduler_overhead_total;
                uint64_t idle_time = total_elapsed_time - total_work_time;
                float idle_percentage = ((float)idle_time / (float)total_elapsed_time) * 100.0f;


                // --- แสดงผลสถิติ ---
                ESP_LOGI(TAG, "=== Statistics for %u ms Time Slice ===", current_time_slice_ms);
                ESP_LOGI(TAG, "Total elapsed time: %.2f s", total_elapsed_sec);
                ESP_LOGI(TAG, "Total switches: %lu", context_switches);
                
                // ค่าเฉลี่ยต่อ 1 รอบ
                ESP_LOGI(TAG, "Avg time per task: %llu us", avg_task_time_us);
                ESP_LOGI(TAG, "Avg time per overhead: %llu us", avg_overhead_time_us);
                
                // ผลลัพธ์สำคัญสำหรับเปรียบเทียบกับตาราง
                ESP_LOGW(TAG, "Context Switches/sec: %.1f", context_switches_per_sec);
                ESP_LOGW(TAG, "CPU utilization (Task work): %.1f%%", cpu_utilization);
                ESP_LOGW(TAG, "Overhead percentage: %.1f%%", overhead_percentage);
                ESP_LOGI(TAG, "Idle percentage (vTaskDelay): %.1f%%", idle_percentage);

            }

            // หยุดพัก 2 วินาที ก่อนเริ่มการทดสอบ Time Slice ถัดไป
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "Experiment loop finished. Restarting in 10 seconds...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
