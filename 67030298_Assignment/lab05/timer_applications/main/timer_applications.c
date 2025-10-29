/* * =============================================================
 * ESP32 Advanced FreeRTOS Timers & System Integration
 * * Implements 4 challenges:
 * 1. Advanced Patterns (PWM/LEDC)
 * 2. Multi-Sensor with Queue Sets
 * 3. Network Watchdog (Wi-Fi/UDP)
 * 4. Pattern Learning (NVS)
 * =============================================================
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h" // --- CHALLENGE 3 ---

// ESP-IDF Drivers
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "driver/ledc.h"         // --- CHALLENGE 1 ---
#include "esp_random.h"
#include "esp_system.h"

// Networking (CHALLENGE 3)
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

// Storage (CHALLENGE 4)
#include "nvs.h"

static const char *TAG = "TIMER_APPS_ADV";

// ================ DEFINITIONS ================

// Pin Definitions
#define STATUS_LED       GPIO_NUM_2
#define WATCHDOG_LED     GPIO_NUM_4
#define PATTERN_LED_1    GPIO_NUM_5     // Now controlled by LEDC
#define PATTERN_LED_2    GPIO_NUM_18    // Now controlled by LEDC
#define PATTERN_LED_3    GPIO_NUM_19    // Now controlled by LEDC
#define SENSOR_POWER     GPIO_NUM_21
#define TEMP_SENSOR_PIN  ADC1_CHANNEL_0 // GPIO 36
#define LIGHT_SENSOR_PIN ADC1_CHANNEL_3 // GPIO 39 (--- CHALLENGE 2 ---)

// Timer Periods
#define WATCHDOG_TIMEOUT_MS     10000   // 10 seconds (Increased for network)
#define WATCHDOG_FEED_MS        3000    // Attempt feed every 3 seconds
#define PATTERN_BASE_MS         500     // Base pattern timing
#define TEMP_SAMPLE_MS          1000    // Temp sensor sampling rate
#define LIGHT_SAMPLE_MS         2500    // Light sensor sampling rate
#define STATUS_UPDATE_MS        5000    // Status update interval

// Pattern Types (CHALLENGE 1: Added BREATHE)
typedef enum {
    PATTERN_OFF = 0,
    PATTERN_SLOW_BLINK,
    PATTERN_FAST_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_RAINBOW,
    PATTERN_BREATHE, // <--- CHALLENGE 1
    PATTERN_MAX
} led_pattern_t;

// Sensor Data Structure
typedef struct {
    float value;
    uint32_t timestamp;
    bool valid;
    const char* sensor_name; // --- CHALLENGE 2 ---
} sensor_data_t;

// System Health Structure
typedef struct {
    uint32_t watchdog_feeds;
    uint32_t watchdog_timeouts;
    uint32_t pattern_changes;
    uint32_t sensor_readings;
    uint32_t system_uptime_sec;
    bool system_healthy;
    bool wifi_connected; // --- CHALLENGE 3 ---
} system_health_t;

// NVS Storage (CHALLENGE 4)
#define NVS_NAMESPACE "storage"
#define NVS_KEY "pat_counts"

// Network (CHALLENGE 3)
#define NETWORK_HEARTBEAT_IP   "8.8.8.8" // Google DNS
#define NETWORK_HEARTBEAT_PORT 12345

// ================ GLOBAL VARIABLES ================

// Timers
TimerHandle_t watchdog_timer;
TimerHandle_t feed_timer;
TimerHandle_t pattern_timer;
TimerHandle_t temp_sensor_timer;  // Renamed
TimerHandle_t light_sensor_timer; // --- CHALLENGE 2 ---
TimerHandle_t status_timer;

// Queues & Sets
QueueHandle_t high_prio_sensor_queue; // --- CHALLENGE 2 ---
QueueHandle_t low_prio_sensor_queue;  // --- CHALLENGE 2 ---
QueueHandle_t network_queue;          // --- CHALLENGE 3 ---
QueueSetHandle_t sensor_queue_set;    // --- CHALLENGE 2 ---

// Wi-Fi (CHALLENGE 3)
EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// System State
led_pattern_t current_pattern = PATTERN_OFF;
system_health_t health_stats = {0, 0, 0, 0, 0, true, false};
uint32_t pattern_usage_counts[PATTERN_MAX] = {0}; // --- CHALLENGE 4 ---

// Pattern state
typedef struct {
    int step;
    int direction;
    int intensity; // Used for PWM
    bool state;
} pattern_state_t;
pattern_state_t pattern_state = {0, 1, 0, false};

// ADC
esp_adc_cal_characteristics_t *adc_chars;

// ================ FUNCTION PROTOTYPES ================
void recovery_callback(TimerHandle_t timer);
void change_led_pattern(led_pattern_t new_pattern);
void network_task(void *parameter);
void init_wifi(void);
void save_pattern_counts_to_nvs(void);
void load_pattern_counts_from_nvs(void);
void light_sensor_timer_callback(TimerHandle_t timer);


// ================ NVS (CHALLENGE 4) ================

void load_pattern_counts_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: Error opening NVS handle (%s)", esp_err_to_name(err));
        return;
    }

    size_t required_size = sizeof(pattern_usage_counts);
    err = nvs_get_blob(nvs_handle, NVS_KEY, pattern_usage_counts, &required_size);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS: Successfully loaded pattern counts from NVS");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS: Pattern counts not found. Initializing to zero.");
    } else {
        ESP_LOGE(TAG, "NVS: Error reading blob (%s)", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
}

void save_pattern_counts_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: Error opening NVS handle (%s)", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY, pattern_usage_counts, sizeof(pattern_usage_counts));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: Error writing blob (%s)", esp_err_to_name(err));
    } else {
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS: Error committing NVS (%s)", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "NVS: Successfully saved pattern counts to NVS");
        }
    }
    
    nvs_close(nvs_handle);
}

// ================ NETWORK (CHALLENGE 3) ================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        health_stats.wifi_connected = false;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi disconnected, attempting reconnect...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        health_stats.wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void init_wifi(void) {
    // 1. Init NVS (needed for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Init Netif
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 3. Init Event Loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 4. Create STA
    esp_netif_create_default_wifi_sta();
    
    // 5. Init Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 6. Register Handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 7. Configure STA
    wifi_config_t wifi_config = {
        .sta = {
            // ===================================================
            .ssid = "YOUR_WIFI_SSID",     // <--- !!! EDIT THIS !!!
            .password = "YOUR_WIFI_PASSWORD", // <--- !!! EDIT THIS !!!
            // ===================================================
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    
    // 8. Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "init_wifi finished. Waiting for connection...");
}

void network_task(void *parameter) {
    ESP_LOGI(TAG, "Network Task started. Waiting for Wi-Fi...");
    
    // Wait for Wi-Fi connection
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    
    ESP_LOGI(TAG, "Wi-Fi connected. Network Task operational.");

    // Setup UDP socket
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(NETWORK_HEARTBEAT_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(NETWORK_HEARTBEAT_PORT);
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // Set a timeout for send/recv (non-blocking is also an option)
    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    char payload = 'H'; // 'H' for Heartbeat
    bool cmd;
    
    while (1) {
        // Wait for a command from the feed_timer
        if (xQueueReceive(network_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            
            bool success = false;
            if (health_stats.wifi_connected) {
                ESP_LOGI(TAG, "🌐 Sending network heartbeat to %s...", NETWORK_HEARTBEAT_IP);
                
                int err = sendto(sock, &payload, sizeof(payload), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                
                if (err < 0) {
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    success = false;
                } else {
                    ESP_LOGI(TAG, "Network heartbeat sent successfully");
                    success = true; // For UDP, "sent" is our best guess of success
                }
            } else {
                ESP_LOGW(TAG, "Cannot send heartbeat, Wi-Fi is disconnected.");
            }

            // *** THIS IS THE CORE LOGIC ***
            // Only reset the watchdog if the network operation succeeded
            if (success) {
                ESP_LOGI(TAG, "🍖 Network feed SUCCESS! Resetting watchdog.");
                xTimerReset(watchdog_timer, 0); 
                health_stats.watchdog_feeds++;
            } else {
                ESP_LOGE(TAG, "🔥 Network feed FAILED! Watchdog NOT reset.");
                // If this continues, watchdog_timeout_callback will fire.
            }
        }
    }
}


// ================ WATCHDOG SYSTEM (CHALLENGE 3 Modified) ================

void watchdog_timeout_callback(TimerHandle_t timer) {
    health_stats.watchdog_timeouts++;
    health_stats.system_healthy = false;
    
    ESP_LOGE(TAG, "🚨 WATCHDOG TIMEOUT! Network feed failed or system hung!");
    ESP_LOGE(TAG, "System stats: Feeds=%lu, Timeouts=%lu", 
             health_stats.watchdog_feeds, health_stats.watchdog_timeouts);
    
    // Flash watchdog LED rapidly
    for (int i = 0; i < 10; i++) {
        gpio_set_level(WATCHDOG_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(WATCHDOG_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    ESP_LOGW(TAG, "In production: esp_restart() would be called here");
    xTimerReset(watchdog_timer, 0); // Reset for demo purposes
    health_stats.system_healthy = true;
}

void feed_watchdog_callback(TimerHandle_t timer) {
    static int feed_count = 0;
    feed_count++;
    
    // Simulate occasional system hangs
    if (feed_count == 15) {
        ESP_LOGW(TAG, "🐛 Simulating system hang - stopping watchdog feeds for 12 seconds");
        xTimerStop(feed_timer, 0);
        
        TimerHandle_t recovery_timer = xTimerCreate("Recovery", 
                                                  pdMS_TO_TICKS(12000),
                                                  pdFALSE, // One-shot
                                                  (void*)0,
                                                  recovery_callback);
        xTimerStart(recovery_timer, 0);
        return;
    }
    
    // --- MODIFIED LOGIC (CHALLENGE 3) ---
    // Instead of resetting the watchdog directly,
    // send a command to the network_task to do it.
    
    ESP_LOGI(TAG, "Attempting to feed watchdog (via network task)...");
    
    bool send_cmd = true;
    BaseType_t higher_priority_task_woken = pdFALSE;
    
    // Send command to network task
    if (xQueueSendFromISR(network_queue, &send_cmd, &higher_priority_task_woken) != pdTRUE) {
        ESP_LOGW(TAG, "Network queue full! Cannot send heartbeat command.");
    }
    
    // Flash status LED briefly
    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(STATUS_LED, 0);
    
    portYIELD_FROM_ISR(higher_priority_task_woken);
    // *** Note: xTimerReset() is GONE from here! ***
}

void recovery_callback(TimerHandle_t timer) {
    ESP_LOGI(TAG, "🔄 System recovered - resuming watchdog feed attempts");
    xTimerStart(feed_timer, 0);
    xTimerDelete(timer, 0);
}

// ================ LED PATTERN (CHALLENGE 1 Modified) ================

// Helper function to set LEDC (PWM) duty
void set_pattern_leds_pwm(uint32_t led1_duty, uint32_t led2_duty, uint32_t led3_duty) {
    // 10-bit resolution = 0-1023
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, led1_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, led2_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, led3_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

void pattern_timer_callback(TimerHandle_t timer) {
    static uint32_t pattern_cycle = 0;
    pattern_cycle++;
    
    // 10-bit resolution (0-1023)
    const uint32_t MAX_BRIGHTNESS = 1023;
    
    switch (current_pattern) {
        case PATTERN_OFF:
            set_pattern_leds_pwm(0, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            break;
            
        case PATTERN_SLOW_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds_pwm(pattern_state.state ? MAX_BRIGHTNESS : 0, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            ESP_LOGI(TAG, "💡 Slow Blink: %s", pattern_state.state ? "ON" : "OFF");
            break;
            
        case PATTERN_FAST_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds_pwm(0, pattern_state.state ? MAX_BRIGHTNESS : 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(200), 0);
            break;
            
        case PATTERN_HEARTBEAT: {
            int step = pattern_state.step % 10;
            bool pulse = (step < 2) || (step >= 3 && step < 5);
            set_pattern_leds_pwm(0, 0, pulse ? MAX_BRIGHTNESS : 0);
            pattern_state.step++;
            xTimerChangePeriod(timer, pdMS_TO_TICKS(100), 0);
            if (step == 9) ESP_LOGI(TAG, "💓 Heartbeat pulse");
            break;
        }
        
        case PATTERN_SOS: {
            static const char* sos = "...---...";
            static int sos_pos = 0;
            
            bool on = (sos[sos_pos] == '.');
            int duration = on ? 200 : 600; 
            
            set_pattern_leds_pwm(on ? MAX_BRIGHTNESS : 0, on ? MAX_BRIGHTNESS : 0, on ? MAX_BRIGHTNESS : 0);
            
            sos_pos = (sos_pos + 1) % strlen(sos);
            if (sos_pos == 0) {
                ESP_LOGI(TAG, "🆘 SOS Pattern Complete");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            xTimerChangePeriod(timer, pdMS_TO_TICKS(duration), 0);
            break;
        }
        
        case PATTERN_RAINBOW: {
            int rainbow_step = pattern_state.step % 8;
            bool led1 = (rainbow_step & 1) != 0;
            bool led2 = (rainbow_step & 2) != 0;
            bool led3 = (rainbow_step & 4) != 0;
            
            set_pattern_leds_pwm(led1 ? MAX_BRIGHTNESS : 0, led2 ? MAX_BRIGHTNESS : 0, led3 ? MAX_BRIGHTNESS : 0);
            pattern_state.step++;
            
            if (rainbow_step == 7) ESP_LOGI(TAG, "🌈 Rainbow cycle complete");
            xTimerChangePeriod(timer, pdMS_TO_TICKS(300), 0);
            break;
        }

        // --- CHALLENGE 1: NEW PATTERN ---
        case PATTERN_BREATHE: {
            // Update intensity based on direction
            pattern_state.intensity += (pattern_state.direction * 10); // Step
            
            // Check bounds and reverse direction
            if (pattern_state.intensity >= MAX_BRIGHTNESS) {
                pattern_state.intensity = MAX_BRIGHTNESS;
                pattern_state.direction = -1; // Fade out
            } else if (pattern_state.intensity <= 0) {
                pattern_state.intensity = 0;
                pattern_state.direction = 1;  // Fade in
                ESP_LOGI(TAG, "💨 Breathe cycle complete");
            }
            
            // Set all LEDs to the same breathing intensity
            set_pattern_leds_pwm(pattern_state.intensity, pattern_state.intensity, pattern_state.intensity);
            
            // Use a fast timer period for smooth fading
            xTimerChangePeriod(timer, pdMS_TO_TICKS(20), 0); 
            break;
        }
        
        default:
            set_pattern_leds_pwm(0, 0, 0);
            break;
    }
    
    // Change pattern every 50 cycles
    if (pattern_cycle % 50 == 0) {
        led_pattern_t new_pattern = (current_pattern + 1) % PATTERN_MAX;
        change_led_pattern(new_pattern);
    }
}

// --- CHALLENGE 4: Modified Function ---
void change_led_pattern(led_pattern_t new_pattern) {
    const char* pattern_names[] = {
        "OFF", "SLOW_BLINK", "FAST_BLINK", 
        "HEARTBEAT", "SOS", "RAINBOW", "BREATHE" // Added
    };
    
    if (new_pattern >= PATTERN_MAX) new_pattern = PATTERN_OFF;
    
    ESP_LOGI(TAG, "🎨 Changing pattern: %s -> %s", 
             pattern_names[current_pattern], pattern_names[new_pattern]);
    
    // --- CHALLENGE 4: Pattern Learning Logic ---
    pattern_usage_counts[new_pattern]++;
    ESP_LOGI(TAG, "NVS: Pattern %s usage count: %lu", 
             pattern_names[new_pattern], pattern_usage_counts[new_pattern]);
    // ------------------------------------------

    current_pattern = new_pattern;
    pattern_state.step = 0;
    pattern_state.state = false;
    pattern_state.intensity = 0;
    pattern_state.direction = 1;
    health_stats.pattern_changes++;
    
    xTimerReset(pattern_timer, 0);
}

// ================ SENSOR (CHALLENGE 2 Modified) ================

float read_temp_sensor_value(void) {
    uint32_t adc_reading = adc1_get_raw(TEMP_SENSOR_PIN);
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
    float sensor_value = (voltage / 1000.0) * 50.0; // 0-50°C range
    sensor_value += (esp_random() % 100 - 50) / 100.0;
    return sensor_value;
}

float read_light_sensor_value(void) {
    uint32_t adc_reading = adc1_get_raw(LIGHT_SENSOR_PIN);
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
    float sensor_value = (voltage / 3300.0) * 1000.0; // 0-1000 Lux range (example)
    sensor_value += (esp_random() % 100 - 50) / 10.0;
    if (sensor_value < 0) sensor_value = 0;
    return sensor_value;
}

// --- CHALLENGE 2: High Priority Sensor Timer ---
void temp_sensor_timer_callback(TimerHandle_t timer) {
    // Enable sensor power
    gpio_set_level(SENSOR_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10)); // Power stabilization

    sensor_data_t sensor_data;
    sensor_data.value = read_temp_sensor_value();
    sensor_data.timestamp = xTaskGetTickCount();
    sensor_data.valid = (sensor_data.value >= 0 && sensor_data.value <= 50);
    sensor_data.sensor_name = "Temperature";
    
    health_stats.sensor_readings++;
    
    // Disable sensor power
    gpio_set_level(SENSOR_POWER, 0);

    // Send to HIGH priority queue
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (xQueueSendFromISR(high_prio_sensor_queue, &sensor_data, &higher_priority_task_woken) != pdTRUE) {
        ESP_LOGW(TAG, "HIGH Prio Sensor queue full - dropping sample");
    }
    
    // Adaptive sampling
    TickType_t new_period = (sensor_data.value > 40.0) ? pdMS_TO_TICKS(500) : pdMS_TO_TICKS(1000);
    xTimerChangePeriodFromISR(timer, new_period, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

// --- CHALLENGE 2: Low Priority Sensor Timer ---
void light_sensor_timer_callback(TimerHandle_t timer) {
    // (Assume sensor is always on or shares power)
    sensor_data_t sensor_data;
    sensor_data.value = read_light_sensor_value();
    sensor_data.timestamp = xTaskGetTickCount();
    sensor_data.valid = (sensor_data.value >= 0 && sensor_data.value <= 1000);
    sensor_data.sensor_name = "Light";

    health_stats.sensor_readings++;

    // Send to LOW priority queue
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (xQueueSendFromISR(low_prio_sensor_queue, &sensor_data, &higher_priority_task_woken) != pdTRUE) {
        ESP_LOGW(TAG, "LOW Prio Sensor queue full - dropping sample");
    }
    
    portYIELD_FROM_ISR(higher_priority_task_woken);
}


// ================ STATUS SYSTEM ================

void status_timer_callback(TimerHandle_t timer) {
    health_stats.system_uptime_sec = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    
    ESP_LOGI(TAG, "\n═══════ SYSTEM STATUS ═══════");
    ESP_LOGI(TAG, "Uptime: %lu seconds", health_stats.system_uptime_sec);
    ESP_LOGI(TAG, "System Health: %s", health_stats.system_healthy ? "✅ HEALTHY" : "❌ ISSUES");
    ESP_LOGI(TAG, "Wi-Fi: %s", health_stats.wifi_connected ? "✅ CONNECTED" : "❌ DISCONNECTED");
    ESP_LOGI(TAG, "Watchdog Feeds (Net): %lu", health_stats.watchdog_feeds);
    ESP_LOGI(TAG, "Watchdog Timeouts: %lu", health_stats.watchdog_timeouts);
    ESP_LOGI(TAG, "Pattern Changes: %lu", health_stats.pattern_changes);
    ESP_LOGI(TAG, "Sensor Readings: %lu", health_stats.sensor_readings);
    ESP_LOGI(TAG, "Current Pattern: %d", current_pattern);
    
    ESP_LOGI(TAG, "Timer States:");
    ESP_LOGI(TAG, "  Watchdog: %s", xTimerIsTimerActive(watchdog_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Feed: %s", xTimerIsTimerActive(feed_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Pattern: %s", xTimerIsTimerActive(pattern_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Temp Sensor: %s", xTimerIsTimerActive(temp_sensor_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Light Sensor: %s", xTimerIsTimerActive(light_sensor_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "════════════════════════════\n");
    
    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(STATUS_LED, 0);
}

// ================ PROCESSING TASKS ================

// --- CHALLENGE 2: Modified Task using Queue Sets ---
void sensor_processing_task(void *parameter) {
    sensor_data_t sensor_data;
    float temp_sum = 0;
    int sample_count = 0;
    
    ESP_LOGI(TAG, "Sensor processing task (Multi-Queue) started");
    
    while (1) {
        // Wait for data on *any* queue in the set
        QueueHandle_t active_queue = (QueueHandle_t)xQueueSelectFromSet(sensor_queue_set, portMAX_DELAY);

        // *** PRIORITY LOGIC ***
        // Always check the high-priority queue first, even if the low-priority queue triggered the set.
        
        // 1. Check HIGH Prio Queue (Temperature)
        if (uxQueueMessagesWaiting(high_prio_sensor_queue) > 0) {
             if (xQueueReceive(high_prio_sensor_queue, &sensor_data, 0) == pdTRUE) {
                 if (sensor_data.valid) {
                    temp_sum += sensor_data.value;
                    sample_count++;
                    
                    ESP_LOGI(TAG, "🌡️ [HIGH] %s: %.2f°C", sensor_data.sensor_name, sensor_data.value);
                    
                    // Process moving average
                    if (sample_count >= 10) {
                        float average = temp_sum / sample_count;
                        ESP_LOGI(TAG, "📊 Temperature Average: %.2f°C", average);
                        if (average > 35.0) {
                            ESP_LOGW(TAG, "🔥 High temperature warning!");
                            change_led_pattern(PATTERN_FAST_BLINK);
                        } else if (average < 15.0) {
                            ESP_LOGW(TAG, "🧊 Low temperature warning!");
                            change_led_pattern(PATTERN_SOS);
                        }
                        temp_sum = 0;
                        sample_count = 0;
                    }
                 } else {
                     ESP_LOGW(TAG, "Invalid temp sensor reading: %.2f", sensor_data.value);
                 }
             }
        } 
        
        // 2. Check LOW Prio Queue (Light) only if HIGH is empty
        else if (active_queue == low_prio_sensor_queue) {
             if (xQueueReceive(low_prio_sensor_queue, &sensor_data, 0) == pdTRUE) {
                 if (sensor_data.valid) {
                     ESP_LOGI(TAG, "☀️ [LOW] %s: %.1f Lux", sensor_data.sensor_name, sensor_data.value);
                     // Just log light level, no complex processing
                     if (sensor_data.value < 50) {
                         // change_led_pattern(PATTERN_BREATHE); // Example action
                     }
                 }
             }
        }
    }
}

// --- CHALLENGE 4: Modified Task ---
void system_monitor_task(void *parameter) {
    ESP_LOGI(TAG, "System monitor task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Every 1 minute
        
        // Check system health
        if (health_stats.watchdog_timeouts > 5) {
            ESP_LOGE(TAG, "🚨 Too many watchdog timeouts - system unstable!");
            health_stats.system_healthy = false;
        }
        
        // Check sensor health
        static uint32_t last_sensor_count = 0;
        if (health_stats.sensor_readings == last_sensor_count) {
            ESP_LOGW(TAG, "⚠️ Sensor readings stopped - checking sensor system");
        }
        last_sensor_count = health_stats.sensor_readings;
        
        // Memory health check
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "💾 Free heap: %d bytes", free_heap);
        if (free_heap < 20000) { // Increased threshold
            ESP_LOGW(TAG, "⚠️ Low memory warning!");
        }

        // --- CHALLENGE 4: Periodic NVS Save ---
        static int save_counter_min = 0;
        save_counter_min++;
        if (save_counter_min >= 5) { // Save every 5 minutes
            ESP_LOGI(TAG, "NVS: 5-minute interval reached. Saving pattern data...");
            save_pattern_counts_to_nvs();
            save_counter_min = 0;
        }
        // ------------------------------------
    }
}

// ================ INITIALIZATION ================

// --- CHALLENGE 1: Modified Function ---
void init_hardware(void) {
    // Configure simple output pins
    gpio_set_direction(STATUS_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(WATCHDOG_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(SENSOR_POWER, GPIO_MODE_OUTPUT);
    
    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(WATCHDOG_LED, 0);
    gpio_set_level(SENSOR_POWER, 0);

    // --- CHALLENGE 1: Configure LEDC (PWM) ---
    ESP_LOGI(TAG, "Initializing LEDC (PWM) for pattern LEDs...");
    
    // 1. Configure Timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_10_BIT, // 0-1023
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 2. Configure Channel 0 (LED 1)
    ledc_channel_config_t ledc_channel_0 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PATTERN_LED_1,
        .duty       = 0,
        .hpoint     = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_0));

    // 3. Configure Channel 1 (LED 2)
    ledc_channel_config_t ledc_channel_1 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PATTERN_LED_2,
        .duty       = 0,
        .hpoint     = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_1));

    // 4. Configure Channel 2 (LED 3)
    ledc_channel_config_t ledc_channel_2 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_2,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PATTERN_LED_3,
        .duty       = 0,
        .hpoint     = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_2));
    // ------------------------------------

    // --- CHALLENGE 2: Configure ADCs ---
    ESP_LOGI(TAG, "Initializing ADCs...");
    adc1_config_width(ADC_WIDTH_BIT_12);
    
    // Temp Sensor
    adc1_config_channel_atten(TEMP_SENSOR_PIN, ADC_ATTEN_DB_12);
    // Light Sensor
    adc1_config_channel_atten(LIGHT_SENSOR_PIN, ADC_ATTEN_DB_12);
    
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, adc_chars);
    // ------------------------------------
    
    ESP_LOGI(TAG, "Hardware initialization complete");
}

void create_timers(void) {
    watchdog_timer = xTimerCreate("WatchdogTimer", pdMS_TO_TICKS(WATCHDOG_TIMEOUT_MS), pdFALSE, (void*)1, watchdog_timeout_callback);
    feed_timer = xTimerCreate("FeedTimer", pdMS_TO_TICKS(WATCHDOG_FEED_MS), pdTRUE, (void*)2, feed_watchdog_callback);
    pattern_timer = xTimerCreate("PatternTimer", pdMS_TO_TICKS(PATTERN_BASE_MS), pdTRUE, (void*)3, pattern_timer_callback);
    status_timer = xTimerCreate("StatusTimer", pdMS_TO_TICKS(STATUS_UPDATE_MS), pdTRUE, (void*)5, status_timer_callback);

    // --- CHALLENGE 2 ---
    temp_sensor_timer = xTimerCreate("TempSensorTimer", pdMS_TO_TICKS(TEMP_SAMPLE_MS), pdTRUE, (void*)4, temp_sensor_timer_callback);
    light_sensor_timer = xTimerCreate("LightSensorTimer", pdMS_TO_TICKS(LIGHT_SAMPLE_MS), pdTRUE, (void*)6, light_sensor_timer_callback);

    if (!watchdog_timer || !feed_timer || !pattern_timer || !temp_sensor_timer || !status_timer || !light_sensor_timer) {
        ESP_LOGE(TAG, "Failed to create one or more timers");
    } else {
        ESP_LOGI(TAG, "All timers created successfully");
    }
}

// --- CHALLENGE 2 & 3: Modified Function ---
void create_queues_and_sets(void) {
    // Challenge 2: Sensor Queues
    high_prio_sensor_queue = xQueueCreate(20, sizeof(sensor_data_t));
    low_prio_sensor_queue = xQueueCreate(20, sizeof(sensor_data_t));
    
    // Challenge 3: Network Queue
    network_queue = xQueueCreate(5, sizeof(bool));
    
    if (!high_prio_sensor_queue || !low_prio_sensor_queue || !network_queue) {
        ESP_LOGE(TAG, "Failed to create one or more queues");
        return;
    }
    
    // Challenge 2: Queue Set
    sensor_queue_set = xQueueCreateSet(20 + 20); // Size = sum of member queues
    if (!sensor_queue_set) {
        ESP_LOGE(TAG, "Failed to create queue set");
        return;
    }
    
    // Add sensor queues to the set
    xQueueAddToSet(high_prio_sensor_queue, sensor_queue_set);
    xQueueAddToSet(low_prio_sensor_queue, sensor_queue_set);

    ESP_LOGI(TAG, "Queues and Queue Set created successfully");
}

void start_system(void) {
    ESP_LOGI(TAG, "Starting timer system...");
    
    xTimerStart(watchdog_timer, 0);
    xTimerStart(feed_timer, 0);
    xTimerStart(pattern_timer, 0);
    xTimerStart(temp_sensor_timer, 0);
    xTimerStart(light_sensor_timer, 0);
    xTimerStart(status_timer, 0);
    
    // Create processing tasks
    xTaskCreate(sensor_processing_task, "SensorProc", 3072, NULL, 6, NULL);
    xTaskCreate(system_monitor_task, "SysMonitor", 3072, NULL, 3, NULL);
    xTaskCreate(network_task, "NetworkTask", 4096, NULL, 5, NULL); // --- CHALLENGE 3 ---
    
    ESP_LOGI(TAG, "🚀 Advanced Timer System Started!");
}

// ================ MAIN ================

void app_main(void) {
    ESP_LOGI(TAG, "Timer Applications Lab (Advanced) Starting...");
    
    // Create event group *before* init_wifi
    wifi_event_group = xEventGroupCreate();

    // Init Wi-Fi (which also inits NVS)
    init_wifi(); // --- CHALLENGE 3 ---
    
    // Load learned patterns *after* NVS is init
    load_pattern_counts_from_nvs(); // --- CHALLENGE 4 ---

    // Initialize hardware
    init_hardware();
    create_queues_and_sets();
    create_timers();
    
    // Start the system
    start_system();
    
    // Start with a cool pattern
    change_led_pattern(PATTERN_BREATHE);
    
    ESP_LOGI(TAG, "System operational - monitoring started");
}
