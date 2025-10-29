#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h" // เพิ่ม header สำหรับ esp_timer_get_time()

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
#define BUTTON_PIN GPIO_NUM_0

static const char *TAG = "SINGLE_TASK_TIMED";

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

    ESP_LOGI(TAG, "Single Task System Started (with timing)");

    bool button_pressed_flag = false; // Flag ป้องกันการกดค้างแล้วแสดงผลซ้ำ
    uint64_t button_press_time = 0;   // เวลาที่กดปุ่ม

    while (1) {
        // --- ส่วน Task 1: Blink LED1 เหมือนเดิม ---
        ESP_LOGD(TAG, "Reading sensor..."); // ใช้ LOGD เพื่อลด Log ที่ไม่จำเป็น
        gpio_set_level(LED1_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));

        // --- ส่วน Task 2: Process data เหมือนเดิม ---
        ESP_LOGD(TAG, "Processing data...");
        for (int i = 0; i < 1000000; i++) {
            volatile int dummy = i * i;
        }

        // --- ส่วน Task 3: Control LED2 เหมือนเดิม ---
        ESP_LOGD(TAG, "Controlling actuator...");
        gpio_set_level(LED2_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(300));

        // --- ส่วน Task 4: Check button (เพิ่มการจับเวลา) ---
        if (gpio_get_level(BUTTON_PIN) == 0 && !button_pressed_flag) {
            // ปุ่มถูกกดครั้งแรก
            button_press_time = esp_timer_get_time(); // บันทึกเวลาที่กด (microseconds)
            button_pressed_flag = true;
            ESP_LOGW(TAG, "Button press detected!");
        } else if (gpio_get_level(BUTTON_PIN) == 1 && button_pressed_flag) {
            // ปุ่มถูกปล่อยหลังจากกด
            uint64_t current_time = esp_timer_get_time();
            uint64_t response_time_us = current_time - button_press_time; // คำนวณเวลาตอบสนอง (us)
            uint32_t response_time_ms = (uint32_t)(response_time_us / 1000); // แปลงเป็น ms

            ESP_LOGW(TAG, "Button released. Response time: %llu us (~%lu ms)", response_time_us, response_time_ms);
            button_pressed_flag = false; // รีเซ็ต flag
        }
    }
}
