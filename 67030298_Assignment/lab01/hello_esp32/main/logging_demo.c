#include <stdio.h>
#include <stdarg.h> // เพิ่ม include สำหรับ va_list
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h" // เพิ่ม include สำหรับ esp_timer_get_time()

// Define tag for logging
static const char *TAG = "LOGGING_DEMO";

// --- โค้ดจาก Step 2 เดิม ---
void demonstrate_logging_levels(void)
{
    ESP_LOGE(TAG, "This is an ERROR message - highest priority");
    ESP_LOGW(TAG, "This is a WARNING message");
    ESP_LOGI(TAG, "This is an INFO message - default level");
    ESP_LOGD(TAG, "This is a DEBUG message - needs debug level");
    ESP_LOGV(TAG, "This is a VERBOSE message - needs verbose level");
}

void demonstrate_formatted_logging(void)
{
    int temperature = 25;
    float voltage = 3.3;
    const char* status = "OK";

    ESP_LOGI(TAG, "Sensor readings:");
    ESP_LOGI(TAG, "  Temperature: %d°C", temperature);
    ESP_LOGI(TAG, "  Voltage: %.2fV", voltage);
    ESP_LOGI(TAG, "  Status: %s", status);

    // Hexadecimal dump
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ESP_LOGI(TAG, "Data dump:");
    ESP_LOG_BUFFER_HEX(TAG, data, sizeof(data));
}

void demonstrate_conditional_logging(void)
{
    int error_code = 0;

    // Conditional logging
    if (error_code != 0) {
        ESP_LOGE(TAG, "Error occurred: code %d", error_code);
    } else {
        ESP_LOGI(TAG, "System is running normally");
    }

    // Using ESP_ERROR_CHECK
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");
}
// --- สิ้นสุดโค้ดจาก Step 2 เดิม ---

// --- โค้ดสำหรับ Exercise 1: สร้าง Custom Logger ---
#define LOG_COLOR_BLACK   "30"
#define LOG_COLOR_RED     "31"
#define LOG_COLOR_GREEN   "32"
#define LOG_COLOR_BROWN   "33"
#define LOG_COLOR_BLUE    "34"
#define LOG_COLOR_PURPLE  "35"
#define LOG_COLOR_CYAN    "36"

#define LOG_BOLD(COLOR)  "\033[1;" COLOR "m"
#define LOG_RESET_COLOR  "\033[0m"

void custom_log(const char* tag, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    printf(LOG_BOLD(LOG_COLOR_CYAN) "[CUSTOM] %s: " LOG_RESET_COLOR, tag);
    vprintf(format, args);
    printf("\n");

    va_end(args);
}
// --- สิ้นสุด Exercise 1 ---

// --- โค้ดสำหรับ Exercise 2: Performance Monitoring ---
void performance_demo(void)
{
    ESP_LOGI(TAG, "\n--- Performance Monitoring Demo ---");

    // Measure execution time
    uint64_t start_time = esp_timer_get_time();

    // Simulate some work
    for (int i = 0; i < 1000000; i++) {
        volatile int dummy = i * 2;
    }

    uint64_t end_time = esp_timer_get_time();
    uint64_t execution_time = end_time - start_time;

    ESP_LOGI(TAG, "Execution time: %llu microseconds", execution_time);
    ESP_LOGI(TAG, "Execution time: %.2f milliseconds", execution_time / 1000.0);
}
// --- สิ้นสุด Exercise 2 ---

// --- โค้ดสำหรับ Exercise 3: Error Handling Demo ---
void error_handling_demo(void)
{
    ESP_LOGI(TAG, "\n--- Error Handling Demo ---");

    // Simulate various error conditions
    esp_err_t result;

    // Success case
    result = ESP_OK;
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Operation completed successfully");
    }

    // Error cases
    result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        // แสดงชื่อ Error แทนที่จะเป็นแค่ตัวเลข
        ESP_LOGE(TAG, "Error: %s", esp_err_to_name(result));
    }

    result = ESP_ERR_INVALID_ARG;
    // ตรวจสอบ Error แต่ไม่หยุดโปรแกรม (แค่แสดง Log)
    ESP_ERROR_CHECK_WITHOUT_ABORT(result);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Non-fatal error: %s", esp_err_to_name(result));
    }
}
// --- สิ้นสุด Exercise 3 ---


void app_main(void)
{
    // System information
    ESP_LOGI(TAG, "=== ESP32 Hello World Demo ===");
    esp_log_level_set("LOGGING_DEMO", ESP_LOG_DEBUG);
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip Model: %s", CONFIG_IDF_TARGET);

    // CPU and Flash info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip cores: %d", chip_info.cores);
    uint32_t flash_size = 0;
    esp_err_t ret_flash = esp_flash_get_size(NULL, &flash_size);
    if (ret_flash != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size (%s)", esp_err_to_name(ret_flash));
    } else {
        ESP_LOGI(TAG, "Flash size: %luMB %s",
                 flash_size / (1024 * 1024),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }
    ESP_LOGI(TAG, "Free Heap: %lu bytes", (long unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Min Free Heap: %lu bytes", (long unsigned int)esp_get_minimum_free_heap_size());

    // Demonstrate different logging levels
    ESP_LOGI(TAG, "\n--- Logging Levels Demo ---");
    ESP_LOGD(TAG, "This Debug message should appear if log level is Debug or Verbose.");
    demonstrate_logging_levels();

    // Demonstrate formatted logging
    ESP_LOGI(TAG, "\n--- Formatted Logging Demo ---");
    demonstrate_formatted_logging();

    // Demonstrate conditional logging
    ESP_LOGI(TAG, "\n--- Conditional Logging Demo ---");
    demonstrate_conditional_logging();

    // --- เรียกใช้ฟังก์ชันจากแบบฝึกหัด ---
    // Exercise 1:
    custom_log("MY_TAG", "This is a custom log message with temperature: %d", 30);

    // Exercise 2:
    performance_demo();

    // Exercise 3:
    error_handling_demo();
    // --- สิ้นสุดการเรียกใช้แบบฝึกหัด ---

    // Main loop with counter
    int counter = 0;
    while (1) {
        ESP_LOGI(TAG, "Main loop iteration: %d", counter++);

        // Log memory status every 10 iterations
        if (counter % 10 == 0) {
            ESP_LOGI(TAG, "Memory status - Free: %lu bytes", (long unsigned int)esp_get_free_heap_size());
        }

        // Simulate different log levels based on counter
        if (counter % 20 == 0) {
            ESP_LOGW(TAG, "Warning: Counter reached %d", counter);
        }

        if (counter > 50) {
            ESP_LOGE(TAG, "Error simulation: Counter exceeded 50!");
            counter = 0; // Reset counter
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // 2 seconds
    }
}
