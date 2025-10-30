/*
 * Exercise 3: ESP32 Peripheral Integration
 *
 * 1. WiFi connectivity (event-driven)
 * 2. Hardware timer triggering periodic tasks
 * 3. GPIO interrupt handling (Button)
 * 4. SPI/I2C task (Simulated)
 * 5. Resource sharing (Mutex)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h" // สำหรับ WiFi

// 1. WiFi
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

// 2. Hardware Timer
#include "driver/gptimer.h"

// 3. GPIO
#include "driver/gpio.h"

// --- WiFi Config ---
#define WIFI_SSID      "FBT3505"
#define WIFI_PASSWORD  "0909910079"

// --- GPIO Config ---
#define LED_GPIO       GPIO_NUM_2  // LED บนบอร์ด (หรือ GPIO 2)
#define BUTTON_GPIO    GPIO_NUM_0  // ปุ่ม BOOT/FLASH บนบอร์ด

static const char *TAG = "EX3_PERIPHERALS";

// 1. WiFi Event Group
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// 2. Hardware Timer Semaphore (From ISR)
static SemaphoreHandle_t timer_semaphore;

// 3. GPIO Interrupt Semaphore (From ISR)
static SemaphoreHandle_t button_semaphore;

// 5. Mutex for shared resource
static SemaphoreHandle_t shared_spi_bus_mutex;

// --- 3. GPIO ISR Handler ---
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    BaseType_t high_task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(button_semaphore, &high_task_awoken);
    // (ไม่จำเป็นต้อง return high_task_awoken ใน esp-idf isr)
}

// --- 2. Timer ISR Callback ---
static bool IRAM_ATTR timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data)
{
    BaseType_t high_task_awoken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_data;
    xSemaphoreGiveFromISR(sem, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}


// --- 1. WiFi Event Handler ---
void wifi_event_handler(void* arg, esp_event_base_t event_base,
                       int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying WiFi connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- 1. WiFi Init Task ---
void wifi_init_task(void *pvParameters)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init task finished.");

    // รอเชื่อมต่อ
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to WiFi network!");

    vTaskDelete(NULL); // ลบ Task นี้ทิ้งเมื่อเชื่อมต่อสำเร็จ
}

// --- 2. Hardware Timer Task ---
void timer_event_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Timer Event Task: Started on Core %d", xPortGetCoreID());
    bool led_state = false;

    while (1)
    {
        // รอ Semaphore จาก Timer ISR (ทุก 1 วินาที)
        if (xSemaphoreTake(timer_semaphore, portMAX_DELAY) == pdTRUE)
        {
            led_state = !led_state;
            gpio_set_level(LED_GPIO, led_state);
            ESP_LOGI(TAG, "Timer Tick (Core %d) - LED %s",
                     xPortGetCoreID(), led_state ? "ON" : "OFF");
        }
    }
}

// --- 3. Button Interrupt Task ---
void button_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Button Task: Started on Core %d. Press BOOT button.", xPortGetCoreID());

    while (1)
    {
        // รอ Semaphore จาก Button ISR
        if (xSemaphoreTake(button_semaphore, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGW(TAG, "Button Pressed! (Core %d)", xPortGetCoreID());
            // Debounce delay
            vTaskDelay(pdMS_TO_TICKS(200));
            // เคลียร์ semaphore ที่อาจเกิดขึ้นระหว่าง debounce
            xSemaphoreTake(button_semaphore, (TickType_t)0);
        }
    }
}

// --- 4 & 5. SPI/I2C Task with Mutex ---
void spi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "SPI Task: Started on Core %d", xPortGetCoreID());

    while(1)
    {
        // 5. ล็อค Mutex ก่อนใช้ SPI bus
        if (xSemaphoreTake(shared_spi_bus_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            ESP_LOGI(TAG, "SPI Task (Core %d): Got Mutex, writing to SPI device...", xPortGetCoreID());
            // จำลองการทำงาน SPI
            vTaskDelay(pdMS_TO_TICKS(50));
            
            // 5. ปลดล็อค Mutex
            xSemaphoreGive(shared_spi_bus_mutex);
            ESP_LOGI(TAG, "SPI Task (Core %d): Released Mutex.", xPortGetCoreID());
        } else {
            ESP_LOGE(TAG, "SPI Task: Failed to get Mutex!");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- Setup Functions ---
void setup_gpio(void)
{
    // 2. LED GPIO
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    // 3. Button GPIO
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(BUTTON_GPIO, GPIO_INTR_NEGEDGE); // Interrupt ขอบขาลง

    // Install ISR service
    gpio_install_isr_service(0);
    // Hook ISR handler
    gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, (void *)BUTTON_GPIO);
}

void setup_hardware_timer(void)
{
    // 2. Hardware Timer
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1MHz, 1 tick = 1µs
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_callback,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, timer_semaphore));

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = 1000000, // 1,000,000 µs = 1 วินาที
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}


void app_main(void)
{
    // Initialize NVS (จำเป็นสำหรับ WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting Exercise 3: Peripheral Integration");

    // สร้าง Semaphores และ Mutex
    timer_semaphore = xSemaphoreCreateBinary();
    button_semaphore = xSemaphoreCreateBinary();
    shared_spi_bus_mutex = xSemaphoreCreateMutex();

    // ตั้งค่า Hardware
    setup_gpio();
    setup_hardware_timer();

    // 1. สร้าง WiFi Task (Core 1)
    xTaskCreatePinnedToCore(wifi_init_task, "WiFiTask", 4096, NULL, 10, NULL, APP_CPU_NUM);

    // 2. สร้าง Timer Event Task (Core 0, Real-time)
    xTaskCreatePinnedToCore(timer_event_task, "TimerTask", 2048, NULL, 15, NULL, PRO_CPU_NUM);

    // 3. สร้าง Button Task (Core 1, I/O)
    xTaskCreatePinnedToCore(button_task, "ButtonTask", 2048, NULL, 10, NULL, APP_CPU_NUM);
    
    // 4 & 5. สร้าง SPI Task (No Affinity)
    xTaskCreate(spi_task, "SPITask", 2048, NULL, 5, NULL);
}
