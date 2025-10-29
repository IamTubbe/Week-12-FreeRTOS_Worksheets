#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "driver/gpio.h"
// สำหรับ Challenge 2: Networking
#include "esp_wifi.h"
#include "esp_event.h"
#include "mqtt_client.h" // ตรวจสอบว่าได้ include "esp_mqtt.h" หรือ "mqtt_client.h" ถูกต้อง

static const char *TAG = "ADV_TIMERS";

// ================ CONFIGURATION & NETWORK SETUP ================
#define IS_MASTER_NODE              1       
#define NODE_ID                     "WORKER_A" 
#define MQTT_BROKER_URI             "mqtt://broker.hivemq.com" 
#define TOPIC_BASE                  "adv_timers/job/"

#define WIFI_SSID                   "YOUR_WIFI_SSID" 
#define WIFI_PASS                   "YOUR_WIFI_PASSWORD" 

// Scheduler & Pool Config
#define TIMER_POOL_SIZE             20
#define PERFORMANCE_BUFFER_SIZE     100
#define HEALTH_CHECK_INTERVAL       1000
#define NUM_PRIORITY_LEVELS         3
#define NUM_WORKER_TASKS            2
#define JOB_QUEUE_LENGTH            50

// Adaptive Governor Config (Challenge 3)
#define MAX_DEADLINE_MISSES         50
#define THROTTLE_FACTOR             1.5

// LEDs
#define PERFORMANCE_LED     GPIO_NUM_2
#define HEALTH_LED          GPIO_NUM_4
#define STRESS_LED          GPIO_NUM_5
#define ERROR_LED           GPIO_NUM_18

// ================ DATA STRUCTURES ================

typedef enum {
    PRIO_HIGH = 0,
    PRIO_MEDIUM,
    PRIO_LOW
} job_priority_t;

typedef struct {
    TimerCallbackFunction_t original_callback; 
    TimerHandle_t timer_handle;             
    uint32_t timer_id;                      
    TickType_t deadline_ticks;              
    char worker_target[16];                 
} job_t;

typedef struct {
    TimerHandle_t handle;
    bool in_use;
    uint32_t id;
    char name[16];
    TickType_t period;
    TickType_t default_period;           
    bool is_throttled;                   
    bool auto_reload;
    TimerCallbackFunction_t callback; 
    void* context;                   
    uint32_t creation_time;
    uint32_t start_count;
    uint32_t callback_count;
    job_priority_t priority;
    TickType_t deadline_ticks;
    char target_node[16];               
} timer_pool_entry_t;

typedef struct {
    uint32_t total_timers_created;
    uint32_t active_timers;
    uint32_t pool_utilization;
    uint32_t failed_creations;
    uint32_t callback_overruns;      
    uint32_t command_failures;       
    uint32_t deadline_misses;        
    float average_accuracy;
    uint32_t free_heap_bytes;
    uint32_t queue_high_watermarks[NUM_PRIORITY_LEVELS]; 
} timer_health_t;

typedef struct {
    uint32_t callback_start_time;
    uint32_t callback_duration_us;
    uint32_t timer_id;
    BaseType_t service_task_priority; 
    uint32_t queue_length;           
    bool accuracy_ok;
} performance_sample_t;

// ================ FUNCTION PROTOTYPES (แก้ไข Implicit Declaration Errors) ================

void init_hardware(void);
void init_timer_pool(void);
void init_monitoring(void);
void create_system_timers(void);

// Timer Callbacks (Job Functions)
void performance_test_callback(TimerHandle_t timer);
void stress_test_callback(TimerHandle_t timer);
void health_monitor_callback(TimerHandle_t timer);

// Scheduler & Pool Management
timer_pool_entry_t* allocate_from_pool(const char* name, TickType_t period, 
                                       bool auto_reload, TimerCallbackFunction_t callback,
                                       job_priority_t priority, uint32_t deadline_ms,
                                       const char* target_node);
void release_to_pool(uint32_t timer_id);
void scheduler_timer_callback(TimerHandle_t timer);
void worker_task(void *pvParameters);

// Monitoring & Adaptive (C3)
void record_performance_sample(uint32_t timer_id, uint32_t duration_us, bool accuracy_ok);
void analyze_performance(void);
void adaptive_governor_check(void);
void performance_analysis_task(void *parameter);
void stress_test_task(void *parameter);


// ================ GLOBAL VARIABLES ================

timer_pool_entry_t timer_pool[TIMER_POOL_SIZE];
SemaphoreHandle_t pool_mutex;
uint32_t next_timer_id = 1000;

performance_sample_t perf_buffer[PERFORMANCE_BUFFER_SIZE];
uint32_t perf_buffer_index = 0;
SemaphoreHandle_t perf_mutex;

timer_health_t health_data = {0};
TimerHandle_t health_monitor_timer;

QueueHandle_t work_queues[NUM_PRIORITY_LEVELS];
TaskHandle_t worker_task_handles[NUM_WORKER_TASKS];
TaskHandle_t stress_test_task_handle;

// C2 Globals
esp_mqtt_client_handle_t mqtt_client = NULL;
SemaphoreHandle_t wifi_connected_sem;

// ================ NETWORK & MQTT (C2) ================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            if (!IS_MASTER_NODE) {
                char topic[64];
                snprintf(topic, sizeof(topic), "%s%s", TOPIC_BASE, NODE_ID);
                esp_mqtt_client_subscribe(mqtt_client, topic, 1);
                ESP_LOGI(TAG, "Subscribed to: %s", topic);
            }
            break;
        case MQTT_EVENT_DATA:
            if (!IS_MASTER_NODE) {
                // WORKER RECEIVES JOB COMMAND
                uint32_t timer_id = atoi(event->data);
                
                job_t received_job = {
                    // **แก้ไข:** performance_test_callback ถูกประกาศแล้ว
                    .original_callback = performance_test_callback, 
                    .timer_id = timer_id,
                    .deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(100), 
                };

                if (xQueueSend(work_queues[PRIO_HIGH], &received_job, 0) != pdTRUE) {
                     health_data.command_failures++;
                     ESP_LOGE(TAG, "Worker Job Queue Full!");
                }
            }
            break;
        default:
            break;
    }
}

void mqtt_app_start(void) {
    // **แก้ไข:** ใช้โครงสร้าง broker.uri แทน uri
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI, // <--- แก้ไขตรงนี้
        .credentials.client_id = NODE_ID,      // <--- แก้ไขตรงนี้ (ใช้ .credentials.client_id)
    };
    // **แก้ไข:** เพิ่มวงเล็บปีกกาที่ขาดหายไปใน initializer (Werror=missing-braces)
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ... (ส่วน WiFi เหมือนเดิม) ...
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xSemaphoreGive(wifi_connected_sem);
    }
}

void wifi_init_sta(void) {
    wifi_connected_sem = xSemaphoreCreateBinary();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID, 
            .password = WIFI_PASS,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    if(xSemaphoreTake(wifi_connected_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        ESP_LOGI(TAG, "WiFi connected!");
    } else {
        ESP_LOGE(TAG, "WiFi connection failed!");
    }
}


// ================ TIMER POOL MANAGEMENT (C1 & C3) ================

void init_timer_pool(void) {
    pool_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        memset(&timer_pool[i], 0, sizeof(timer_pool_entry_t));
    }
    ESP_LOGI(TAG, "Timer pool initialized with %d slots", TIMER_POOL_SIZE);
}

timer_pool_entry_t* allocate_from_pool(const char* name, TickType_t period, 
                                       bool auto_reload, TimerCallbackFunction_t callback,
                                       job_priority_t priority, uint32_t deadline_ms,
                                       const char* target_node) {
    
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return NULL; }
    timer_pool_entry_t* entry = NULL;
    
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        if (!timer_pool[i].in_use) {
            entry = &timer_pool[i];
            entry->in_use = true;
            entry->id = next_timer_id++;
            strncpy(entry->name, name, sizeof(entry->name) - 1);
            entry->period = period;
            entry->default_period = period; 
            entry->is_throttled = false;    
            entry->auto_reload = auto_reload;
            entry->callback = callback;
            entry->priority = priority;
            entry->deadline_ticks = pdMS_TO_TICKS(deadline_ms);
            strncpy(entry->target_node, target_node, sizeof(entry->target_node) - 1); 

            // **แก้ไข:** scheduler_timer_callback ถูกประกาศแล้ว
            entry->handle = xTimerCreate(name, period, auto_reload, 
                                       (void*)entry->id, scheduler_timer_callback); 
            
            if (entry->handle == NULL) { entry->in_use = false; entry = NULL; health_data.failed_creations++; } 
            else { health_data.total_timers_created++; }
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
    return entry;
}

void release_to_pool(uint32_t timer_id) {
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return; }
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        if (timer_pool[i].in_use && timer_pool[i].id == timer_id) {
            if (timer_pool[i].handle) {
                xTimerDelete(timer_pool[i].handle, 0);
            }
            timer_pool[i].in_use = false;
            ESP_LOGI(TAG, "Released timer %lu from pool", timer_id);
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
}

// ================ SCHEDULER CALLBACK & WORKER TASKS (C1 & C2) ================

void scheduler_timer_callback(TimerHandle_t timer) {
    uint32_t timer_id = (uint32_t)pvTimerGetTimerID(timer);
    
    if (xSemaphoreTake(pool_mutex, 0) != pdTRUE) { health_data.command_failures++; return; }
    timer_pool_entry_t* entry = NULL;
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        if (timer_pool[i].in_use && timer_pool[i].id == timer_id) { entry = &timer_pool[i]; break; }
    }
    
    if (entry) {
        if (IS_MASTER_NODE && strcmp(entry->target_node, NODE_ID) != 0) {
            // C2: MASTER NODE ส่ง Job ไปยัง Worker Node อื่นผ่าน MQTT
            char topic[64];
            char payload[16];
            snprintf(topic, sizeof(topic), "%s%s", TOPIC_BASE, entry->target_node);
            snprintf(payload, sizeof(payload), "%lu", entry->id);

            int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
            if (msg_id == -1) {
                health_data.command_failures++;
                ESP_LOGE(TAG, "Master failed to publish Job %lu to %s", entry->id, entry->target_node);
            } else {
                ESP_LOGD(TAG, "Master published Job %lu to %s", entry->id, entry->target_node);
            }
        } else {
            // C1: Local Execution 
            job_t job = {
                .original_callback = entry->callback,
                .timer_handle = entry->handle,
                .timer_id = entry->id,
                .deadline_ticks = xTaskGetTickCount() + entry->deadline_ticks,
                .worker_target[0] = '\0'
            };

            if (xQueueSend(work_queues[entry->priority], &job, 0) != pdTRUE) {
                health_data.command_failures++;
                gpio_set_level(ERROR_LED, 1);
                ESP_LOGE(TAG, "Scheduler queue full for priority %d", entry->priority);
            }
        }
    }
    xSemaphoreGive(pool_mutex);
}

void worker_task(void *pvParameters) {
    job_t job;
    while (1) {
        bool job_found = false;
        
        for (int prio = PRIO_HIGH; prio <= PRIO_LOW; prio++) {
            TickType_t wait_ticks = (prio == PRIO_LOW) ? pdMS_TO_TICKS(100) : 0;
            
            if (xQueueReceive(work_queues[prio], &job, wait_ticks) == pdTRUE) {
                job_found = true;
                
                // C1: Deadline Monitoring
                if (xTaskGetTickCount() > job.deadline_ticks) {
                    health_data.deadline_misses++;
                    ESP_LOGW(TAG, "Deadline missed for timer ID %lu", job.timer_id);
                }
                
                if (job.original_callback) {
                    job.original_callback(job.timer_handle);
                }
                break;
            }
        }
        if (!job_found) { continue; }
    }
}

// ================ PERFORMANCE & CALLBACKS ================

void record_performance_sample(uint32_t timer_id, uint32_t duration_us, bool accuracy_ok) {
    if (xSemaphoreTake(perf_mutex, 0) == pdTRUE) {
        performance_sample_t* sample = &perf_buffer[perf_buffer_index];
        sample->timer_id = timer_id;
        sample->callback_duration_us = duration_us;
        sample->accuracy_ok = accuracy_ok;
        sample->callback_start_time = esp_timer_get_time() / 1000;
        sample->service_task_priority = uxTaskPriorityGet(NULL); 
        perf_buffer_index = (perf_buffer_index + 1) % PERFORMANCE_BUFFER_SIZE;
        if (duration_us > 1000) { health_data.callback_overruns++; }
        xSemaphoreGive(perf_mutex);
    }
}

void analyze_performance(void) {
    if (xSemaphoreTake(perf_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return; }
    
    uint32_t total_duration = 0;
    uint32_t max_duration = 0;
    uint32_t min_duration = UINT32_MAX;
    uint32_t accurate_timers = 0;
    uint32_t sample_count = 0;
    
    for (int i = 0; i < PERFORMANCE_BUFFER_SIZE; i++) {
        if (perf_buffer[i].callback_duration_us > 0) {
            total_duration += perf_buffer[i].callback_duration_us;
            if (perf_buffer[i].callback_duration_us > max_duration) max_duration = perf_buffer[i].callback_duration_us;
            if (perf_buffer[i].callback_duration_us < min_duration) min_duration = perf_buffer[i].callback_duration_us;
            if (perf_buffer[i].accuracy_ok) accurate_timers++;
            sample_count++;
        }
    }
    
    if (sample_count > 0) {
        uint32_t avg_duration = total_duration / sample_count;
        health_data.average_accuracy = (float)accurate_timers / sample_count * 100.0;
        
        ESP_LOGI(TAG, "📊 Performance Analysis: Avg=%luμs, Max=%luμs, Acc=%.1f%%", 
                 avg_duration, max_duration, health_data.average_accuracy);
        
        if (avg_duration > 500) { gpio_set_level(PERFORMANCE_LED, 1); } 
        else { gpio_set_level(PERFORMANCE_LED, 0); }
    }
    xSemaphoreGive(perf_mutex);
}

void performance_test_callback(TimerHandle_t timer) {
    uint32_t start_time = esp_timer_get_time();
    uint32_t timer_id = (uint32_t)pvTimerGetTimerID(timer);
    
    // Simulate work
    volatile uint32_t iterations = 100 + (esp_random() % 500);
    for (volatile uint32_t i = 0; i < iterations; i++) {}
    
    uint32_t end_time = esp_timer_get_time();
    uint32_t duration_us = end_time - start_time;
    
    // Simplified Accuracy Check
    bool accuracy_ok = true; 
    
    record_performance_sample(timer_id, duration_us, accuracy_ok);
    
    // Update timer stats
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < TIMER_POOL_SIZE; i++) {
            if (timer_pool[i].in_use && timer_pool[i].id == timer_id) {
                timer_pool[i].callback_count++;
                break;
            }
        }
        xSemaphoreGive(pool_mutex);
    }
}

void stress_test_callback(TimerHandle_t timer) {
    static uint32_t stress_counter = 0;
    stress_counter++;
    if (stress_counter % 100 == 0) {
        ESP_LOGI(TAG, "💪 Stress test callback #%lu (on %s)", 
                 stress_counter, pcTaskGetName(NULL));
        gpio_set_level(STRESS_LED, (stress_counter/100) % 2);
    }
}

void health_monitor_callback(TimerHandle_t timer) {
    health_data.free_heap_bytes = esp_get_free_heap_size();
    
    uint32_t active_count = 0;
    uint32_t pool_used = 0;
    
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < TIMER_POOL_SIZE; i++) {
            if (timer_pool[i].in_use) {
                pool_used++;
                if (xTimerIsTimerActive(timer_pool[i].handle)) { active_count++; }
            }
        }
        xSemaphoreGive(pool_mutex);
    }
    
    for(int i=0; i<NUM_PRIORITY_LEVELS; i++) {
        health_data.queue_high_watermarks[i] = uxQueueSpacesAvailable(work_queues[i]);
    }
    
    health_data.active_timers = active_count;
    health_data.pool_utilization = (pool_used * 100) / TIMER_POOL_SIZE;
    
    if (health_data.pool_utilization > 80 || health_data.deadline_misses > 10) {
        gpio_set_level(HEALTH_LED, 1); 
    } else {
        gpio_set_level(HEALTH_LED, 0);
    }
    
    ESP_LOGD(TAG, "🏥 Health: Active=%lu/%lu, Util=%lu%%, Heap=%lu, Misses=%lu", 
             active_count, pool_used, health_data.pool_utilization, health_data.free_heap_bytes, health_data.deadline_misses);
}

// ================ ADAPTIVE GOVERNOR (C3) ================

void adaptive_governor_check(void) {
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return; }
    
    // A. ภาวะ Overload (ต้องลดโหลด)
    if (health_data.callback_overruns > 10 || health_data.deadline_misses > MAX_DEADLINE_MISSES) {
        ESP_LOGW(TAG, "🤖 Governor: High load detected! Starting throttling.");
        
        for (int i = 0; i < TIMER_POOL_SIZE; i++) {
            if (timer_pool[i].in_use && timer_pool[i].priority == PRIO_LOW && !timer_pool[i].is_throttled) {
                
                TickType_t new_period = (TickType_t)(timer_pool[i].default_period * THROTTLE_FACTOR);
                
                if (xTimerChangePeriod(timer_pool[i].handle, new_period, pdMS_TO_TICKS(10)) == pdPASS) {
                    timer_pool[i].period = new_period;
                    timer_pool[i].is_throttled = true;
                    ESP_LOGW(TAG, "  -> Throttled %s (ID %lu) to %lu ms", 
                             timer_pool[i].name, timer_pool[i].id, pdTICKS_TO_MS(new_period));
                } else { health_data.command_failures++; }
            }
        }
        // Reset counters
        health_data.callback_overruns = 0;
        health_data.deadline_misses = 0;
    } 
    // B. ภาวะ Recovery (ปรับกลับ)
    else if (health_data.callback_overruns == 0 && health_data.deadline_misses == 0) {
        bool needs_reset = false;
        
        for (int i = 0; i < TIMER_POOL_SIZE; i++) {
            if (timer_pool[i].in_use && timer_pool[i].is_throttled) {
                needs_reset = true;
                
                if (xTimerChangePeriod(timer_pool[i].handle, timer_pool[i].default_period, pdMS_TO_TICKS(10)) == pdPASS) {
                    timer_pool[i].period = timer_pool[i].default_period;
                    timer_pool[i].is_throttled = false;
                    ESP_LOGI(TAG, "  -> Recovered %s (ID %lu)", timer_pool[i].name, timer_pool[i].id);
                } else { health_data.command_failures++; }
            }
        }
    }
    xSemaphoreGive(pool_mutex);
}

// ================ STRESS TESTING (C1, C2, C3) ================

void stress_test_task(void *parameter) {
    ESP_LOGI(TAG, "🔥 Starting stress test...");
    
    timer_pool_entry_t* timers[18];
    const char* target = IS_MASTER_NODE ? "WORKER_B" : NODE_ID; // Master ส่งไป Worker B, Worker รันเอง
    
    // สร้าง 15 Low Prio Timers (จะถูก Throttling)
    for (int i = 0; i < 15; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Stress%d", i);
        uint32_t period_ms = 50 + (i * 10);
        
        timers[i] = allocate_from_pool(name, pdMS_TO_TICKS(period_ms), 
                                            true, stress_test_callback,
                                            PRIO_LOW, 300, target);
        if (timers[i] != NULL) { xTimerStart(timers[i]->handle, 0); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // สร้าง 3 Medium Prio Timers (Monitor)
    for (int i = 15; i < 18; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Sensor%d", i);
        timers[i] = allocate_from_pool(name, pdMS_TO_TICKS(500), 
                                            true, performance_test_callback,
                                            PRIO_MEDIUM, 500, NODE_ID); // รัน Local เสมอ
        if (timers[i] != NULL) { xTimerStart(timers[i]->handle, 0); }
    }
    
    vTaskDelay(pdMS_TO_TICKS(30000));
    
    // Clean up
    for (int i = 0; i < 18; i++) {
        if (timers[i] != NULL) {
            xTimerStop(timers[i]->handle, pdMS_TO_TICKS(100));
            release_to_pool(timers[i]->id);
        }
    }
    
    ESP_LOGI(TAG, "Stress test completed");
    vTaskDelete(NULL);
}

// ================ INITIALIZATION ================

void init_hardware(void) {
    gpio_set_direction(PERFORMANCE_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(HEALTH_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(STRESS_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(ERROR_LED, GPIO_MODE_OUTPUT);
    
    gpio_set_level(PERFORMANCE_LED, 0);
    gpio_set_level(HEALTH_LED, 0);
    gpio_set_level(STRESS_LED, 0);
    gpio_set_level(ERROR_LED, 0);
}

void performance_analysis_task(void *parameter) {
    ESP_LOGI(TAG, "Performance analysis task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        analyze_performance();
        adaptive_governor_check(); // C3
        
        // Report
        ESP_LOGI(TAG, "\n═══ PERFORMANCE REPORT ═══");
        ESP_LOGI(TAG, "Active Timers: %lu, Util: %lu%%, Heap: %lu", health_data.active_timers, health_data.pool_utilization, health_data.free_heap_bytes);
        ESP_LOGI(TAG, "Accuracy: %.1f%%, Overruns: %lu, Deadline Misses: %lu", health_data.average_accuracy, health_data.callback_overruns, health_data.deadline_misses);
        ESP_LOGI(TAG, "Queue Spaces (H,M,L): %lu, %lu, %lu", health_data.queue_high_watermarks[0], health_data.queue_high_watermarks[1], health_data.queue_high_watermarks[2]);
        ESP_LOGI(TAG, "═════════════════════════\n");
        
        if (health_data.free_heap_bytes < 20000 || health_data.deadline_misses > 50) {
            gpio_set_level(ERROR_LED, 1);
        } else {
            gpio_set_level(ERROR_LED, 0);
        }
    }
}

void init_monitoring(void) {
    perf_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < NUM_PRIORITY_LEVELS; i++) {
        work_queues[i] = xQueueCreate(JOB_QUEUE_LENGTH, sizeof(job_t));
    }
    ESP_LOGI(TAG, "Monitoring and Job Queues initialized");
}

void app_main(void) {
    ESP_LOGI(TAG, "Advanced Timer System (Node: %s) Starting...", NODE_ID);
    
    // C2: Network Init
    init_hardware(); // **แก้ไข:** เรียกใช้ฟังก์ชัน init_hardware()
    wifi_init_sta();
    mqtt_app_start();
    
    // C1 & C3 Init
    init_timer_pool();
    init_monitoring();
    
    // C1: Worker Tasks
    for (int i = 0; i < NUM_WORKER_TASKS; i++) {
        xTaskCreate(worker_task, "WorkerTask", 3072, (void*)i, 
                    configMAX_PRIORITIES - (i + 2), &worker_task_handles[i]);
    }

    // Health Monitor Timer (รันตรง, ไม่ผ่าน Scheduler)
    health_monitor_timer = xTimerCreate("HealthMonitor", pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL), pdTRUE, (void*)1, health_monitor_callback);
    xTimerStart(health_monitor_timer, 0);
    
    // Performance Analysis Task
    xTaskCreate(performance_analysis_task, "PerfAnalysis", 3072, NULL, 8, NULL);
    
    // Start Stress Test after 5 seconds
    vTaskDelay(pdMS_TO_TICKS(5000));
    xTaskCreate(stress_test_task, "StressTest", 4096, NULL, 5, &stress_test_task_handle);
    
    ESP_LOGI(TAG, "🚀 System Running. Mode: %s", IS_MASTER_NODE ? "MASTER" : "WORKER");
}
