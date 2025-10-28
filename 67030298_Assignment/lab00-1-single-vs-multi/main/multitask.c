#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h" // เพิ่ม header

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
#define BUTTON_PIN GPIO_NUM_0

static const char *TAG = "MULTITASK_TIMED";

// --- Task 1, 2, 3 เหมือนเดิม ---
void sensor_task(void *pvParameters)
{
    while (1) {
        ESP_LOGD(TAG, "Reading sensor...");
        gpio_set_level(LED1_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
}
void processing_task(void *pvParameters)
{
    while (1) {
        ESP_LOGD(TAG, "Processing data...");
        for (int i = 0; i < 500000; i++) {
            volatile int dummy = i * i;
            if (i % 100000 == 0) {
                vTaskDelay(1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
void actuator_task(void *pvParameters)
{
    while (1) {
        ESP_LOGD(TAG, "Controlling actuator...");
        gpio_set_level(LED2_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
}

// --- Task 4: Emergency Response (เพิ่มการจับเวลา) ---
void emergency_task(void *pvParameters)
{
    uint64_t button_press_time = 0;
    bool waiting_for_press = true; // เริ่มต้นรอการกด

    while (1) {
        if (waiting_for_press) {
            if (gpio_get_level(BUTTON_PIN) == 0) {
                // ปุ่มถูกกดครั้งแรก
                button_press_time = esp_timer_get_time(); // บันทึกเวลา
                waiting_for_press = false; // เปลี่ยนสถานะเป็นรอการปล่อย

                // --- ตอบสนองทันที (เหมือนเดิม) ---
                ESP_LOGW(TAG, "EMERGENCY! Button pressed - Responding...");
                gpio_set_level(LED1_PIN, 1);
                gpio_set_level(LED2_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100)); // ไฟติดค้าง 100ms
                gpio_set_level(LED1_PIN, 0);
                gpio_set_level(LED2_PIN, 0);
            }
        } else {
            // กำลังรอการปล่อยปุ่ม
            if (gpio_get_level(BUTTON_PIN) == 1) {
                uint64_t current_time = esp_timer_get_time();
                uint64_t response_time_us = current_time - button_press_time; // คำนวณเวลาตอบสนอง
                uint32_t response_time_ms = (uint32_t)(response_time_us / 1000);

                ESP_LOGW(TAG, "Button released. Response time: %llu us (~%lu ms)", response_time_us, response_time_ms);
                waiting_for_press = true; // กลับไปรอการกดครั้งต่อไป
            }
        }

        // Delay สั้นๆ เพื่อให้ Task อื่นทำงาน และลดการวน Loop ถี่เกินไป
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


void app_main(void)
{
    // --- ส่วน Config GPIO เหมือนเดิม ---
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .pull_up_en = 1,
        .pull_down_en = 0,
    };
    gpio_config(&button_conf);

    ESP_LOGI(TAG, "Multitasking System Started (with timing)");

    // --- ส่วน Create tasks เหมือนเดิม ---
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 2, NULL);
    xTaskCreate(processing_task, "processing", 2048, NULL, 1, NULL);
    xTaskCreate(actuator_task, "actuator", 2048, NULL, 2, NULL);
    xTaskCreate(emergency_task, "emergency", 2048, NULL, 5, NULL);
}
