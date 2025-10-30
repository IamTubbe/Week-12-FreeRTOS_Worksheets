#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"

static const char *TAG = "COMPLEX_EVENTS";

// --- การตั้งค่า Wi-Fi และ MQTT ---
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"

// Topics
#define MQTT_TOPIC_STATUS   "smart_home/status"
#define MQTT_TOPIC_PATTERNS "smart_home/patterns"
#define MQTT_TOPIC_COMMANDS "smart_home/commands"

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;


// --- GPIO, States, Events ---
#define LED_LIVING_ROOM     GPIO_NUM_2      // Living room light
#define LED_KITCHEN         GPIO_NUM_4      // Kitchen light
#define LED_BEDROOM         GPIO_NUM_5      // Bedroom light
#define LED_SECURITY        GPIO_NUM_18     // Security system
#define LED_EMERGENCY       GPIO_NUM_19     // Emergency indicator
#define MOTION_SENSOR       GPIO_NUM_21     // Motion sensor input
#define DOOR_SENSOR         GPIO_NUM_22     // Door sensor input

// Smart Home State Machine States
typedef enum {
    HOME_STATE_IDLE = 0,
    HOME_STATE_OCCUPIED,
    HOME_STATE_AWAY,
    HOME_STATE_SLEEP,
    HOME_STATE_SECURITY_ARMED,
    HOME_STATE_EMERGENCY,
    HOME_STATE_MAINTENANCE
} home_state_t;

// Event Groups และ Event Bits
EventGroupHandle_t sensor_events;
EventGroupHandle_t system_events;
EventGroupHandle_t pattern_events;

// Sensor Events
#define MOTION_DETECTED_BIT     (1 << 0)
#define DOOR_OPENED_BIT         (1 << 1)
#define DOOR_CLOSED_BIT         (1 << 2)
#define LIGHT_ON_BIT            (1 << 3)
#define LIGHT_OFF_BIT           (1 << 4)
#define TEMPERATURE_HIGH_BIT    (1 << 5)
#define TEMPERATURE_LOW_BIT     (1 << 6)
#define SOUND_DETECTED_BIT      (1 << 7)
#define PRESENCE_CONFIRMED_BIT  (1 << 8)

// System Events
#define SYSTEM_INIT_BIT         (1 << 0)
#define USER_HOME_BIT           (1 << 1)
#define USER_AWAY_BIT           (1 << 2)
#define SLEEP_MODE_BIT          (1 << 3)
#define SECURITY_ARMED_BIT      (1 << 4)
#define EMERGENCY_MODE_BIT      (1 << 5)
#define MAINTENANCE_MODE_BIT    (1 << 6)

// Pattern Events
#define PATTERN_NORMAL_ENTRY_BIT    (1 << 0)
#define PATTERN_BREAK_IN_BIT        (1 << 1)
#define PATTERN_EMERGENCY_BIT       (1 << 2)
#define PATTERN_GOODNIGHT_BIT       (1 << 3)
#define PATTERN_WAKE_UP_BIT         (1 << 4)
#define PATTERN_LEAVING_BIT         (1 << 5)
#define PATTERN_RETURNING_BIT       (1 << 6)

// Event และ State Management
static home_state_t current_home_state = HOME_STATE_IDLE;
static SemaphoreHandle_t state_mutex;

// Event History สำหรับ Pattern Recognition
#define EVENT_HISTORY_SIZE 20
typedef struct {
    EventBits_t event_bits;
    uint64_t timestamp;
    home_state_t state_at_time;
} event_record_t;

static event_record_t event_history[EVENT_HISTORY_SIZE];
static int history_index = 0;

// Pattern Recognition Data
typedef struct {
    const char* name;
    EventBits_t required_events[4]; // Up to 4 events in sequence
    uint32_t time_window_ms;        // Max time between events
    EventBits_t result_event;       // Event to set when pattern matches
    void (*action_callback)(void);  // Optional callback function
} event_pattern_t;

// Adaptive System Parameters
typedef struct {
    float motion_sensitivity;
    uint32_t auto_light_timeout;
    uint32_t security_delay;
    bool learning_mode;
    uint32_t pattern_confidence[10];
} adaptive_params_t;

static adaptive_params_t adaptive_params = {
    .motion_sensitivity = 0.7,
    .auto_light_timeout = 300000,   // 5 minutes
    .security_delay = 30000,        // 30 seconds
    .learning_mode = true,
    .pattern_confidence = {0}
};

// Smart Devices Control
typedef struct {
    bool living_room_light;
    bool kitchen_light;
    bool bedroom_light;
    bool security_system;
    bool emergency_mode;
    uint32_t temperature_celsius;
    uint32_t light_level_percent;
} smart_home_status_t;

static smart_home_status_t home_status = {0};

// Advanced Event Correlation Metrics
typedef struct {
    uint32_t total_patterns_detected;
    uint32_t false_positives;
    uint32_t pattern_accuracy[10]; // ขนาดควรตรงกับ pattern_confidence
    float correlation_strength[10]; // ขนาดควรตรงกับ pattern_confidence
    uint32_t adaptive_adjustments;
} pattern_analytics_t;

static pattern_analytics_t analytics = {0};


// --- Prototyping Callbacks and State Functions ---
void normal_entry_action(void);
void break_in_action(void);
void goodnight_action(void);
void wake_up_action(void);
void leaving_action(void);
void returning_action(void);
const char* get_state_name(home_state_t state);
void change_home_state(home_state_t new_state);
void add_event_to_history(EventBits_t event_bits);
void analyze_pattern_performance(void);
void print_event_sequence(void);


// Event Patterns Definition (ลอจิกที่แก้ไขแล้ว)
static event_pattern_t event_patterns[] = {
    {
        // ลำดับที่คาดหวัง: Open (เก่าสุด) -> Motion -> Close (ใหม่สุด)
        .name = "Normal Entry",
        .required_events = {DOOR_CLOSED_BIT, MOTION_DETECTED_BIT, DOOR_OPENED_BIT, 0},
        .time_window_ms = 10000,    // 10 seconds
        .result_event = PATTERN_NORMAL_ENTRY_BIT,
        .action_callback = normal_entry_action
    },
    {
        // ลำดับที่คาดหวัง: Open (เก่าสุด) -> Motion (ใหม่สุด) (ขณะที่ security armed)
        .name = "Break-in Attempt", 
        .required_events = {MOTION_DETECTED_BIT, DOOR_OPENED_BIT, 0, 0},
        .time_window_ms = 5000,     // 5 seconds (when security armed)
        .result_event = PATTERN_BREAK_IN_BIT,
        .action_callback = break_in_action
    },
    {
        // ลำดับที่คาดหวัง: Light Off (เก่าสุด) -> Motion -> Light Off (ใหม่สุด)
        .name = "Goodnight Routine",
        .required_events = {LIGHT_OFF_BIT, MOTION_DETECTED_BIT, LIGHT_OFF_BIT, 0},
        .time_window_ms = 30000,    // 30 seconds
        .result_event = PATTERN_GOODNIGHT_BIT,
        .action_callback = goodnight_action
    },
    {
        // ลำดับที่คาดหวัง: Motion (เก่าสุด) -> Light On (ใหม่สุด) (ขณะที่ sleep mode)
        .name = "Wake-up Routine",
        .required_events = {LIGHT_ON_BIT, MOTION_DETECTED_BIT, 0, 0},
        .time_window_ms = 5000,     // 5 seconds (during sleep mode)
        .result_event = PATTERN_WAKE_UP_BIT,
        .action_callback = wake_up_action
    },
    {
        // ลำดับที่คาดหวัง: Light Off (เก่าสุด) -> Open -> Close (ใหม่สุด)
        .name = "Leaving Home",
        .required_events = {DOOR_CLOSED_BIT, DOOR_OPENED_BIT, LIGHT_OFF_BIT, 0},
        .time_window_ms = 15000,    // 15 seconds
        .result_event = PATTERN_LEAVING_BIT,
        .action_callback = leaving_action
    },
    {
        // ลำดับที่คาดหวัง: Open (เก่าสุด) -> Motion -> Close (ใหม่สุด) (ขณะที่ away/security armed)
        .name = "Returning Home",
        .required_events = {DOOR_CLOSED_BIT, MOTION_DETECTED_BIT, DOOR_OPENED_BIT, 0},
        .time_window_ms = 8000,     // 8 seconds (when security armed)
        .result_event = PATTERN_RETURNING_BIT,
        .action_callback = returning_action
    }
};
#define NUM_PATTERNS (sizeof(event_patterns) / sizeof(event_pattern_t))


// --- ฟังก์ชัน Wi-Fi และ MQTT ---

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected from Wi-Fi, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        // Start MQTT client after getting IP
        if (mqtt_client) {
            esp_mqtt_client_start(mqtt_client);
        }
    }
}

void wifi_init_sta(void) {
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "Wi-Fi initialization finished.");
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;
        // Subscribe to command topic
        esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_COMMANDS, 0);
        ESP_LOGI(TAG, "Subscribed to %s", MQTT_TOPIC_COMMANDS);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "Payload: %.*s", event->data_len, event->data);

        // --- นี่คือส่วนที่รับคำสั่งจาก Mobile App ---
        if (strncmp(event->topic, MQTT_TOPIC_COMMANDS, event->topic_len) == 0) {
            if (strncmp(event->data, "SLEEP", event->data_len) == 0) {
                ESP_LOGI(TAG, "MQTT Command: SLEEP mode");
                xEventGroupSetBits(system_events, SLEEP_MODE_BIT);
            } else if (strncmp(event->data, "AWAY", event->data_len) == 0) {
                ESP_LOGI(TAG, "MQTT Command: AWAY mode");
                xEventGroupSetBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
            } else if (strncmp(event->data, "HOME", event->data_len) == 0) {
                ESP_LOGI(TAG, "MQTT Command: HOME mode");
                xEventGroupSetBits(system_events, USER_HOME_BIT);
                xEventGroupClearBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT | SLEEP_MODE_BIT);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other MQTT event id: %d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void) {
    // สร้าง Client ID สุ่ม
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "esp32-home-%lu", esp_random() % 1000);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = client_id
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    // esp_mqtt_client_start(mqtt_client) ถูกเรียกใน wifi_event_handler
}


// --- Tasks ---

// --- อัปเดต Task: Pattern Recognition ---
void pattern_recognition_task(void *pvParameters) {
    ESP_LOGI(TAG, "🧠 Pattern recognition engine started");
    
    while (1) {
        // Wait for any sensor event
        EventBits_t sensor_bits = xEventGroupWaitBits(
            sensor_events,
            0xFFFFFF,     // Wait for any bit
            pdFALSE,      // Don't clear bits yet
            pdFALSE,      // Wait for any bit (OR condition)
            portMAX_DELAY
        );
        
        if (sensor_bits != 0) {
            ESP_LOGI(TAG, "🔍 Sensor event detected: 0x%08X", sensor_bits);
            
            // Add to history
            add_event_to_history(sensor_bits);
            
            bool pattern_matched_in_cycle = false;

            // Check each pattern
            for (int p = 0; p < NUM_PATTERNS; p++) {
                event_pattern_t* pattern = &event_patterns[p];
                
                // Check if pattern is applicable in current state
                bool state_applicable = true;
                
                if (strcmp(pattern->name, "Break-in Attempt") == 0) {
                    state_applicable = (current_home_state == HOME_STATE_SECURITY_ARMED);
                } else if (strcmp(pattern->name, "Wake-up Routine") == 0) {
                    state_applicable = (current_home_state == HOME_STATE_SLEEP);
                } else if (strcmp(pattern->name, "Returning Home") == 0) {
                    state_applicable = (current_home_state == HOME_STATE_AWAY || current_home_state == HOME_STATE_SECURITY_ARMED);
                }
                
                if (!state_applicable) continue;
                
                // Look for pattern in recent history
                int event_index = 0;
                uint64_t current_time = esp_timer_get_time();
                uint64_t first_event_time = 0; // Timestamp of the oldest event in pattern
                uint64_t last_event_time = 0;  // Timestamp of the newest event in pattern
                
                // Check required events in reverse chronological order (newest to oldest)
                for (int h = 0; h < EVENT_HISTORY_SIZE && pattern->required_events[event_index] != 0; h++) {
                    int hist_idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
                    event_record_t* record = &event_history[hist_idx];
                    
                    if (event_index == 0) {
                        if ((current_time - record->timestamp) > (pattern->time_window_ms * 1000)) {
                             break;
                        }
                    } else {
                        if ((last_event_time - record->timestamp) > (pattern->time_window_ms * 1000)) {
                            break;
                        }
                    }
                    
                    if (record->event_bits & pattern->required_events[event_index]) {
                        if (event_index == 0) last_event_time = record->timestamp;
                        first_event_time = record->timestamp;
                        event_index++;
                        
                        if (pattern->required_events[event_index] == 0) {
                            break;
                        }
                    }
                }
                
                // Check if all required events were found
                if (pattern->required_events[event_index] == 0) {
                    ESP_LOGI(TAG, "🎯 Pattern matched: %s", pattern->name);
                    pattern_matched_in_cycle = true;
                    analytics.total_patterns_detected++;

                    // Set pattern event
                    xEventGroupSetBits(pattern_events, pattern->result_event);
                    
                    // Execute callback if available
                    if (pattern->action_callback) {
                        pattern->action_callback();
                    }

                    // --- ส่วนที่เพิ่มมา: Publish Pattern ขึ้น Cloud ---
                    if (mqtt_connected && mqtt_client) {
                        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_PATTERNS, pattern->name, 0, 1, 0);
                    }
                    // --- จบส่วนที่เพิ่มมา ---
                    
                    if (p < 10) { // Boundary check for analytics arrays
                        adaptive_params.pattern_confidence[p]++;

                        uint64_t duration = last_event_time - first_event_time;
                        float time_window_us = pattern->time_window_ms * 1000.0;
                        if (time_window_us > 0) {
                            analytics.correlation_strength[p] = fmax(0.0, 1.0 - (duration / time_window_us));
                        } else {
                            analytics.correlation_strength[p] = 1.0;
                        }
                        
                        if ((esp_random() % 100) < 90) {
                            analytics.pattern_accuracy[p]++;
                        } else {
                            analytics.false_positives++;
                        }
                    }
                    
                    xEventGroupClearBits(sensor_events, 0xFFFFFF);
                    break;
                }
            } // end for each pattern
            
            if (!pattern_matched_in_cycle) {
                xEventGroupClearBits(sensor_events, sensor_bits);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay
    }
}

// Sensor Simulation Tasks
void motion_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏃 Motion sensor simulation started");
    
    while (1) {
        if ((esp_random() % 100) < 15) { // 15% chance per cycle
            ESP_LOGI(TAG, "👥 Motion detected!");
            xEventGroupSetBits(sensor_events, MOTION_DETECTED_BIT);
            
            vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));
            
            if ((esp_random() % 100) < 60) { // 60% chance of presence confirmation
                ESP_LOGI(TAG, "✅ Presence confirmed");
                xEventGroupSetBits(sensor_events, PRESENCE_CONFIRMED_BIT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000))); // 3-8 seconds
    }
}

void door_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "🚪 Door sensor simulation started");
    bool door_open = false;
    
    while (1) {
        if ((esp_random() % 100) < 8) { // 8% chance per cycle
            if (!door_open) {
                ESP_LOGI(TAG, "🔓 Door opened");
                xEventGroupSetBits(sensor_events, DOOR_OPENED_BIT);
                door_open = true;
                
                vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 8000))); // 2-10 seconds
                
                if ((esp_random() % 100) < 85) { // 85% chance door closes
                    ESP_LOGI(TAG, "🔒 Door closed");
                    xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
                    door_open = false;
                }
            } else {
                ESP_LOGI(TAG, "🔒 Door closed");
                xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
                door_open = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 10000))); // 5-15 seconds
    }
}

void light_control_task(void *pvParameters) {
    ESP_LOGI(TAG, "💡 Light control system started");
    
    while (1) {
        if ((esp_random() % 100) < 12) { // 12% chance per cycle
            bool light_action = (esp_random() % 2);
            
            if (light_action) {
                ESP_LOGI(TAG, "💡 Light turned ON");
                xEventGroupSetBits(sensor_events, LIGHT_ON_BIT);
                int light_choice = esp_random() % 3;
                switch (light_choice) {
                    case 0: 
                        home_status.living_room_light = true;
                        gpio_set_level(LED_LIVING_ROOM, 1);
                        break;
                    case 1:
                        home_status.kitchen_light = true;
                        gpio_set_level(LED_KITCHEN, 1);
                        break;
                    case 2:
                        home_status.bedroom_light = true;
                        gpio_set_level(LED_BEDROOM, 1);
                        break;
                }
            } else {
                ESP_LOGI(TAG, "💡 Light turned OFF");
                xEventGroupSetBits(sensor_events, LIGHT_OFF_BIT);
                int light_choice = esp_random() % 3;
                switch (light_choice) {
                    case 0:
                        home_status.living_room_light = false;
                        gpio_set_level(LED_LIVING_ROOM, 0);
                        break;
                    case 1:
                        home_status.kitchen_light = false;
                        gpio_set_level(LED_KITCHEN, 0);
                        break;
                    case 2:
                        home_status.bedroom_light = false;
                        gpio_set_level(LED_BEDROOM, 0);
                        break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4000 + (esp_random() % 8000))); // 4-12 seconds
    }
}

void environmental_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "🌡️ Environmental sensors started");
    
    while (1) {
        home_status.temperature_celsius = 20 + (esp_random() % 15); // 20-35°C
        
        if (home_status.temperature_celsius > 28) {
            ESP_LOGI(TAG, "🔥 High temperature detected: %d°C", home_status.temperature_celsius);
            xEventGroupSetBits(sensor_events, TEMPERATURE_HIGH_BIT);
        } else if (home_status.temperature_celsius < 22) {
            ESP_LOGI(TAG, "🧊 Low temperature detected: %d°C", home_status.temperature_celsius);
            xEventGroupSetBits(sensor_events, TEMPERATURE_LOW_BIT);
        }
        
        if ((esp_random() % 100) < 5) { // 5% chance
            ESP_LOGI(TAG, "🔊 Sound detected");
            xEventGroupSetBits(sensor_events, SOUND_DETECTED_BIT);
        }
        
        home_status.light_level_percent = esp_random() % 100;
        
        vTaskDelay(pdMS_TO_TICKS(8000 + (esp_random() % 7000))); // 8-15 seconds
    }
}

// State Machine Task
void state_machine_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏠 Home state machine started");
    
    while (1) {
        EventBits_t system_bits = xEventGroupWaitBits(
            system_events,
            0xFFFFFF,
            pdTRUE,       // Clear bits after reading
            pdFALSE,      // Wait for any bit
            pdMS_TO_TICKS(5000) // 5 second timeout
        );
        
        if (system_bits != 0) {
            ESP_LOGI(TAG, "🔄 System event: 0x%08X", system_bits);
            
            if (system_bits & USER_HOME_BIT) {
                if (current_home_state != HOME_STATE_OCCUPIED) {
                    change_home_state(HOME_STATE_OCCUPIED);
                }
            }
            if (system_bits & USER_AWAY_BIT) {
                if (current_home_state != HOME_STATE_AWAY) {
                    change_home_state(HOME_STATE_AWAY);
                }
            }
            if (system_bits & SLEEP_MODE_BIT) {
                if (current_home_state == HOME_STATE_OCCUPIED) {
                    change_home_state(HOME_STATE_SLEEP);
                }
            }
            if (system_bits & SECURITY_ARMED_BIT) {
                if (current_home_state == HOME_STATE_AWAY) {
                    change_home_state(HOME_STATE_SECURITY_ARMED);
                }
            }
            if (system_bits & EMERGENCY_MODE_BIT) {
                change_home_state(HOME_STATE_EMERGENCY);
            }
            if (system_bits & MAINTENANCE_MODE_BIT) {
                change_home_state(HOME_STATE_MAINTENANCE);
            }
        }
        
        // Autonomous state transitions
        switch (current_home_state) {
            case HOME_STATE_EMERGENCY:
                vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds emergency
                ESP_LOGI(TAG, "🆘 Emergency cleared - returning to normal");
                home_status.emergency_mode = false;
                gpio_set_level(LED_EMERGENCY, 0);
                change_home_state(HOME_STATE_OCCUPIED);
                break;
            case HOME_STATE_IDLE:
                EventBits_t sensor_activity = xEventGroupGetBits(sensor_events);
                if (sensor_activity & (MOTION_DETECTED_BIT | PRESENCE_CONFIRMED_BIT)) {
                    change_home_state(HOME_STATE_OCCUPIED);
                }
                break;
            default:
                break;
        }
    }
}

// Adaptive Learning Task
void adaptive_learning_task(void *pvParameters) {
    ESP_LOGI(TAG, "🧠 Adaptive learning system started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Update every 30 seconds
        
        if (adaptive_params.learning_mode) {
            
            // Adaptive motion sensitivity
            uint32_t recent_motion_events = 0;
            uint64_t current_time = esp_timer_get_time();
            
            for (int h = 0; h < EVENT_HISTORY_SIZE; h++) {
                int hist_idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
                event_record_t* record = &event_history[hist_idx];
                
                if ((current_time - record->timestamp) < 300000000) { // Last 5 minutes (300,000,000 us)
                    if (record->event_bits & MOTION_DETECTED_BIT) {
                        recent_motion_events++;
                    }
                } else {
                    break;
                }
            }
            
            // Adjust sensitivity
            bool adjusted = false;
            if (recent_motion_events > 10) {
                adaptive_params.motion_sensitivity *= 0.95; // Reduce sensitivity
                adjusted = true;
            } else if (recent_motion_events < 2) {
                adaptive_params.motion_sensitivity *= 1.05; // Increase sensitivity
                adjusted = true;
            }
            
            // Keep sensitivity in reasonable bounds
            if (adaptive_params.motion_sensitivity > 1.0) adaptive_params.motion_sensitivity = 1.0;
            if (adaptive_params.motion_sensitivity < 0.3) adaptive_params.motion_sensitivity = 0.3;

            if (adjusted) {
                 ESP_LOGI(TAG, "🔧 Adaptive adjustment: Motion sensitivity now %.2f", adaptive_params.motion_sensitivity);
                 analytics.adaptive_adjustments++;
            }
        }
    }
}

// --- อัปเดต Task: Status Monitor ---
void status_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "📊 Status monitor started");
    char status_json_buffer[512]; // Buffer สำหรับสร้าง JSON

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        
        const char* state_name = get_state_name(current_home_state);
        
        // --- ส่วน Log ปกติ (เหมือนเดิม) ---
        ESP_LOGI(TAG, "\n🏠 ═══ SMART HOME STATUS ═══");
        ESP_LOGI(TAG, "Current State:      %s", state_name);
        ESP_LOGI(TAG, "Living Room:        %s", home_status.living_room_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Kitchen:            %s", home_status.kitchen_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Bedroom:            %s", home_status.bedroom_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Security:           %s", home_status.security_system ? "ARMED" : "DISARMED");
        ESP_LOGI(TAG, "Emergency:          %s", home_status.emergency_mode ? "ACTIVE" : "NORMAL");
        ESP_LOGI(TAG, "Temperature:        %lu°C", home_status.temperature_celsius); // (แก้ตรงนี้ด้วย %lu สำหรับ Log)
        ESP_LOGI(TAG, "Light Level:        %lu%%", home_status.light_level_percent); // (แก้ตรงนี้ด้วย %lu สำหรับ Log)
        
        ESP_LOGI(TAG, "\n📊 Event Group Status:");
        ESP_LOGI(TAG, "Sensor Events:      0x%08X", (unsigned int)xEventGroupGetBits(sensor_events)); // (Casting เพื่อความปลอดภัย)
        ESP_LOGI(TAG, "System Events:      0x%08X", (unsigned int)xEventGroupGetBits(system_events));
        ESP_LOGI(TAG, "Pattern Events:     0x%08X", (unsigned int)xEventGroupGetBits(pattern_events));
        
        ESP_LOGI(TAG, "\n🧠 Adaptive Parameters:");
        ESP_LOGI(TAG, "Motion Sensitivity: %.2f", adaptive_params.motion_sensitivity);
        
        // Call the analytics function
        analyze_pattern_performance();

        // Call the event sequence visualization function
        print_event_sequence();
        
        ESP_LOGI(TAG, "\nFree Heap:          %d bytes", (int)esp_get_free_heap_size());
        ESP_LOGI(TAG, "════════════════════════════════════════\n");

        // --- ส่วนที่เพิ่มมา: สร้างและ Publish JSON Status (แก้ไขแล้ว) ---
        if (mqtt_connected && mqtt_client) {
            snprintf(status_json_buffer, sizeof(status_json_buffer),
                "{"
                "\"state\": \"%s\", "
                "\"lights\": {"
                "\"living_room\": %s, \"kitchen\": %s, \"bedroom\": %s"
                "}, "
                "\"security\": {\"armed\": %s, \"emergency\": %s}, "
                "\"sensors\": {\"temperature\": %lu, \"light_level\": %lu}, " // <--- *** แก้ไขแล้ว ***
                "\"analytics\": {\"patterns_detected\": %lu, \"false_positives\": %lu, \"adaptive_adjustments\": %lu}, "
                "\"heap\": %d"
                "}",
                state_name,
                home_status.living_room_light ? "true" : "false",
                home_status.kitchen_light ? "true" : "false",
                home_status.bedroom_light ? "true" : "false",
                home_status.security_system ? "true" : "false",
                home_status.emergency_mode ? "true" : "false",
                home_status.temperature_celsius,      // uint32_t
                home_status.light_level_percent,  // uint32_t
                analytics.total_patterns_detected,
                analytics.false_positives,
                analytics.adaptive_adjustments,
                (int)esp_get_free_heap_size()
            );

            // Publish JSON
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, status_json_buffer, 0, 1, 0);
            ESP_LOGI(TAG, "Published status to MQTT");
        }
        // --- จบส่วนที่เพิ่มมา ---
    }
}

// --- app_main (อัปเดต) ---
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Complex Event Patterns - Smart Home System Starting...");
    
    // --- ส่วนที่เพิ่มมา: Initialize NVS ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // --- จบส่วน NVS ---

    // Configure GPIO
    gpio_set_direction(LED_LIVING_ROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_KITCHEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BEDROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SECURITY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_EMERGENCY, GPIO_MODE_OUTPUT);
    
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 0);
    gpio_set_level(LED_EMERGENCY, 0);
    
    // Create mutex and Event Groups
    state_mutex = xSemaphoreCreateMutex();
    sensor_events = xEventGroupCreate();
    system_events = xEventGroupCreate();
    pattern_events = xEventGroupCreate();
    
    if (!sensor_events || !system_events || !pattern_events || !state_mutex) {
        ESP_LOGE(TAG, "Failed to create RTOS objects!");
        return;
    }
    ESP_LOGI(TAG, "Event groups and mutex created successfully");
    
    // Initialize system state
    xEventGroupSetBits(system_events, SYSTEM_INIT_BIT);
    change_home_state(HOME_STATE_IDLE);
    
    // --- ส่วนที่เพิ่มมา: Start Wi-Fi และ MQTT ---
    ESP_LOGI(TAG, "Initializing Network and MQTT...");
    wifi_init_sta();
    mqtt_app_start();
    // --- จบส่วน Network ---

    // Create tasks
    ESP_LOGI(TAG, "Creating system tasks...");
    xTaskCreate(pattern_recognition_task, "PatternEngine", 4096, NULL, 8, NULL);
    xTaskCreate(state_machine_task, "StateMachine", 3072, NULL, 7, NULL);
    xTaskCreate(adaptive_learning_task, "Learning", 3072, NULL, 5, NULL);
    xTaskCreate(status_monitor_task, "Monitor", 4096, NULL, 3, NULL);
    
    // Sensor simulation tasks
    xTaskCreate(motion_sensor_task, "MotionSensor", 2048, NULL, 6, NULL);
    xTaskCreate(door_sensor_task, "DoorSensor", 2048, NULL, 6, NULL);
    xTaskCreate(light_control_task, "LightControl", 2048, NULL, 6, NULL);
    xTaskCreate(environmental_sensor_task, "EnvSensors", 2048, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "All tasks created successfully");
    
    ESP_LOGI(TAG, "\n🎯 Smart Home LED Indicators:");
    ESP_LOGI(TAG, "   GPIO2  - Living Room Light");
    ESP_LOGI(TAG, "   GPIO4  - Kitchen Light");
    ESP_LOGI(TAG, "   GPIO5  - Bedroom Light");
    ESP_LOGI(TAG, "   GPIO18 - Security System");
    ESP_LOGI(TAG, "   GPIO19 - Emergency Mode");
    
    ESP_LOGI(TAG, "\n🤖 System Features:");
    ESP_LOGI(TAG, "   • Event-driven State Machine");
    ESP_LOGI(TAG, "   • Pattern Recognition Engine (Logic Corrected)");
    ESP_LOGI(TAG, "   • Adaptive Learning System");
    ESP_LOGI(TAG, "   • Complex Event Correlation & Analytics");
    ESP_LOGI(TAG, "   • Event Sequence Visualization");
    ESP_LOGI(TAG, "   • [NEW] Wi-Fi Connected");
    ESP_LOGI(TAG, "   • [NEW] MQTT Client for Cloud/App Integration");
    
    ESP_LOGI(TAG, "\n🔍 Monitored Patterns:");
    for (int i = 0; i < NUM_PATTERNS; i++) {
        ESP_LOGI(TAG, "   • %s", event_patterns[i].name);
    }
    
    ESP_LOGI(TAG, "Complex Event Pattern System operational!");
}

// --- ฟังก์ชัน Helper ทั้งหมด ---

const char* get_state_name(home_state_t state) {
    switch (state) {
        case HOME_STATE_IDLE: return "Idle";
        case HOME_STATE_OCCUPIED: return "Occupied";
        case HOME_STATE_AWAY: return "Away";
        case HOME_STATE_SLEEP: return "Sleep";
        case HOME_STATE_SECURITY_ARMED: return "Security Armed";
        case HOME_STATE_EMERGENCY: return "Emergency";
        case HOME_STATE_MAINTENANCE: return "Maintenance";
        default: return "Unknown";
    }
}

void change_home_state(home_state_t new_state) {
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        home_state_t old_state = current_home_state;
        if (old_state == new_state) {
             xSemaphoreGive(state_mutex);
             return;
        }
        current_home_state = new_state;
        
        ESP_LOGI(TAG, "🏠 State changed: %s → %s", 
                 get_state_name(old_state), get_state_name(new_state));
        
        xSemaphoreGive(state_mutex);
    }
}

void add_event_to_history(EventBits_t event_bits) {
    event_history[history_index].event_bits = event_bits;
    event_history[history_index].timestamp = esp_timer_get_time();
    event_history[history_index].state_at_time = current_home_state;
    
    history_index = (history_index + 1) % EVENT_HISTORY_SIZE;
}

// Pattern Action Callbacks
void normal_entry_action(void) {
    ESP_LOGI(TAG, "🏠 Normal entry pattern detected - Welcome home!");
    home_status.living_room_light = true;
    gpio_set_level(LED_LIVING_ROOM, 1);
    xEventGroupSetBits(system_events, USER_HOME_BIT);
}

void break_in_action(void) {
    ESP_LOGW(TAG, "🚨 Break-in pattern detected - Security alert!");
    home_status.security_system = true;
    home_status.emergency_mode = true;
    gpio_set_level(LED_SECURITY, 1);
    gpio_set_level(LED_EMERGENCY, 1);
    xEventGroupSetBits(system_events, EMERGENCY_MODE_BIT);
}

void goodnight_action(void) {
    ESP_LOGI(TAG, "🌙 Goodnight pattern detected - Sleep mode activated");
    home_status.living_room_light = false;
    home_status.kitchen_light = false;
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 1); // Keep bedroom light dim
    xEventGroupSetBits(system_events, SLEEP_MODE_BIT);
}

void wake_up_action(void) {
    ESP_LOGI(TAG, "☀️ Wake-up pattern detected - Good morning!");
    home_status.bedroom_light = true;
    home_status.kitchen_light = true;
    gpio_set_level(LED_BEDROOM, 1);
    gpio_set_level(LED_KITCHEN, 1);
    xEventGroupClearBits(system_events, SLEEP_MODE_BIT);
}

void leaving_action(void) {
    ESP_LOGI(TAG, "🚪 Leaving pattern detected - Securing home");
    home_status.living_room_light = false;
    home_status.kitchen_light = false;
    home_status.bedroom_light = false;
    home_status.security_system = true;
    
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 1);
    
    xEventGroupSetBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
}

void returning_action(void) {
    ESP_LOGI(TAG, "🔓 Returning pattern detected - Disabling security");
    home_status.security_system = false;
    gpio_set_level(LED_SECURITY, 0);
    xEventGroupClearBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
}

// Analytics/Visualization Functions
void analyze_pattern_performance(void) {
    ESP_LOGI(TAG, "\n📈 ═══ Pattern Analytics ═══");
    ESP_LOGI(TAG, "Total Patterns Detected: %lu", analytics.total_patterns_detected);
    ESP_LOGI(TAG, "Simulated False Positives: %lu", analytics.false_positives);
    ESP_LOGI(TAG, "Adaptive Adjustments: %lu", analytics.adaptive_adjustments);

    for (int i = 0; i < NUM_PATTERNS; i++) {
        if (i < 10 && adaptive_params.pattern_confidence[i] > 0) {
            float accuracy = (float)analytics.pattern_accuracy[i] / 
                           adaptive_params.pattern_confidence[i] * 100.0;
            ESP_LOGI(TAG, "  %s:", event_patterns[i].name);
            ESP_LOGI(TAG, "    Detections: %lu, Accuracy: %.1f%%, Correlation: %.2f", 
                adaptive_params.pattern_confidence[i], accuracy, analytics.correlation_strength[i]);
        }
    }
    ESP_LOGI(TAG, "════════════════════════════════");
}

void print_event_sequence(void) {
    ESP_LOGI(TAG, "\n🕒 Recent Event Sequence (Last 10):");
    uint64_t current_time = esp_timer_get_time();
    
    for (int h = 0; h < 10; h++) { // แสดง 10 เหตุการณ์ล่าสุด
        int hist_idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
        event_record_t* record = &event_history[hist_idx];
        
        if (record->timestamp > 0) { // ป้องกันการพิมพ์ slot ที่ยังว่าง
            uint32_t age_ms = (uint32_t)((current_time - record->timestamp) / 1000);
            ESP_LOGI(TAG, "  [-%5lu ms] State: %-15s, Events: 0x%04X", 
                    age_ms, get_state_name(record->state_at_time), 
                    record->event_bits);
        } else {
            break; // หยุดถ้าเจอประวัติที่ว่าง (ยังไม่เต็ม buffer)
        }
    }
}
