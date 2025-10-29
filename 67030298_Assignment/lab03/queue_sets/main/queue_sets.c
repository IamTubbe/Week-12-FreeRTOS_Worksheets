#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // --- NEW --- for abs()
#include <math.h>   // --- NEW --- for fabs()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "ADV_EVENT_BUS";

// LED indicators
#define LED_SENSOR GPIO_NUM_2
#define LED_USER GPIO_NUM_4
#define LED_NETWORK GPIO_NUM_5
#define LED_TIMER GPIO_NUM_18
#define LED_PROCESSOR GPIO_NUM_19 // Now shared by workers

// --- NEW --- Challenge 3: Load Balancing
#define NUM_WORKERS 2
#define HIGH_PRIO_QUEUE_LEN 10
#define LOW_PRIO_QUEUE_LEN 10

// --- MODIFIED --- Queue handles
QueueHandle_t xSensorQueue;
QueueHandle_t xUserQueue;
QueueHandle_t xNetworkQueue;
SemaphoreHandle_t xTimerSemaphore;

// --- NEW --- Challenge 1 & 3: Worker Queues
QueueHandle_t xHighPrioWorkerQueue;
QueueHandle_t xLowPrioWorkerQueue;

// --- NEW --- Challenge 2: Dynamic Queue
QueueHandle_t xSpecialEventQueue = NULL;

// Queue Set handle
QueueSetHandle_t xQueueSet;

// --- NEW --- Mutexes for thread-safe operations
SemaphoreHandle_t xStatsMutex;    // Protects 'stats' and 'perf_stats'
SemaphoreHandle_t xQueueSetMutex; // Protects adding/removing from QueueSet

// --- MODIFIED --- Data structures (added timestamp)
typedef struct {
    int sensor_id;
    float temperature;
    float humidity;
    uint32_t timestamp;
} sensor_data_t;

typedef struct {
    int button_id;
    bool pressed;
    uint32_t duration_ms;
    uint32_t timestamp; // --- NEW ---
} user_input_t;

typedef struct {
    char source[20];
    char message[100];
    int priority;
    uint32_t timestamp; // --- NEW ---
} network_message_t;

// --- MODIFIED --- Message type identifier
typedef enum {
    MSG_SENSOR,
    MSG_USER,
    MSG_NETWORK,
    MSG_TIMER,
    MSG_SPECIAL // --- NEW --- Challenge 2
} message_type_t;

// --- NEW --- Generic event structure for worker queues
typedef struct {
    message_type_t type;
    uint32_t timestamp; // Promoted for easy access
    union {
        sensor_data_t sensor_data;
        user_input_t user_input;
        network_message_t network_msg;
        // Timer has no data
        int special_event_id; // --- NEW --- Challenge 2
    } data;
} event_message_t;

// --- MODIFIED --- Statistics
typedef struct {
    uint32_t sensor_count;
    uint32_t user_count;
    uint32_t network_count;
    uint32_t timer_count;
    uint32_t special_count; // --- NEW ---
    uint32_t filtered_out;  // --- NEW --- Challenge 4
} message_stats_t;

// --- NEW --- Challenge 5: Performance Metrics
typedef struct {
    uint64_t total_response_time_ms; // Use 64-bit to prevent overflow
    uint32_t total_events_processed;
    uint32_t max_response_time_ms;
} perf_stats_t;

message_stats_t stats = {0, 0, 0, 0, 0, 0};
perf_stats_t perf_stats = {0, 0, 0};

// --- MODIFIED --- Producer tasks now add timestamps
void sensor_task(void *pvParameters) {
    sensor_data_t sensor_data;
    int sensor_id = 1;
    ESP_LOGI(TAG, "Sensor task started");
    while (1) {
        sensor_data.sensor_id = sensor_id;
        sensor_data.temperature = 20.0 + (esp_random() % 200) / 10.0;
        sensor_data.humidity = 30.0 + (esp_random() % 400) / 10.0;
        sensor_data.timestamp = xTaskGetTickCount(); // --- MODIFIED ---
        
        if (xQueueSend(xSensorQueue, &sensor_data, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "📊 Sensor: T=%.1f°C, H=%.1f%%, ID=%d", 
                     sensor_data.temperature, sensor_data.humidity, sensor_id);
            gpio_set_level(LED_SENSOR, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_SENSOR, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000)));
    }
}

void user_input_task(void *pvParameters) {
    user_input_t user_input;
    ESP_LOGI(TAG, "User input task started");
    while (1) {
        user_input.button_id = 1 + (esp_random() % 3);
        user_input.pressed = true;
        user_input.duration_ms = 100 + (esp_random() % 1000);
        user_input.timestamp = xTaskGetTickCount(); // --- NEW ---
        
        if (xQueueSend(xUserQueue, &user_input, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🔘 User: Button %d pressed for %dms", 
                     user_input.button_id, user_input.duration_ms);
            gpio_set_level(LED_USER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_USER, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000)));
    }
}

void network_task(void *pvParameters) {
    network_message_t network_msg;
    const char* sources[] = {"WiFi", "Bluetooth", "LoRa", "Ethernet"};
    const char* messages[] = {"Status update", "Config changed", "Alert", "Sync", "Heartbeat"};
    ESP_LOGI(TAG, "Network task started");
    while (1) {
        strcpy(network_msg.source, sources[esp_random() % 4]);
        strcpy(network_msg.message, messages[esp_random() % 5]);
        network_msg.priority = 1 + (esp_random() % 5);
        network_msg.timestamp = xTaskGetTickCount(); // --- NEW ---
        
        if (xQueueSend(xNetworkQueue, &network_msg, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🌐 Network [%s]: %s (P:%d)", 
                     network_msg.source, network_msg.message, network_msg.priority);
            gpio_set_level(LED_NETWORK, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_NETWORK, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 3000)));
    }
}

// Timer task (no change)
void timer_task(void *pvParameters) {
    ESP_LOGI(TAG, "Timer task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); 
        if (xSemaphoreGive(xTimerSemaphore) == pdPASS) {
            ESP_LOGI(TAG, "⏰ Timer: Periodic timer fired");
            gpio_set_level(LED_TIMER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_TIMER, 0);
        }
    }
}

// --- NEW --- Challenge 2: Dynamic Queue Management Task
void special_event_task(void *pvParameters) {
    ESP_LOGI(TAG, "Special Event task started. Will activate in 30s.");
    vTaskDelay(pdMS_TO_TICKS(30000)); // Wait for 30 seconds

    ESP_LOGW(TAG, "!!! DYNAMIC EVENT: Creating special event queue... !!!");
    xSpecialEventQueue = xQueueCreate(2, sizeof(int));

    if (xSpecialEventQueue) {
        // Must lock the QueueSet while modifying it
        if (xSemaphoreTake(xQueueSetMutex, portMAX_DELAY) == pdTRUE) {
            xQueueAddToSet(xSpecialEventQueue, xQueueSet);
            xSemaphoreGive(xQueueSetMutex);
            ESP_LOGW(TAG, "!!! DYNAMIC EVENT: Queue added to set. Sending event... !!!");
            
            int event_id = 999;
            xQueueSend(xSpecialEventQueue, &event_id, 0);
        }
    }
    vTaskDelete(NULL); // One-shot task
}

// --- NEW --- Helper function to process events (logic from old processor)
void process_event(event_message_t* event, int worker_id) {
    // Turn on processor LED
    gpio_set_level(LED_PROCESSOR, 1);

    // --- NEW --- Challenge 5: Calculate performance metrics
    uint32_t now = xTaskGetTickCount();
    uint32_t response_time_ms = (now - event->timestamp) * portTICK_PERIOD_MS;
    
    // Update global stats safely
    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        perf_stats.total_events_processed++;
        perf_stats.total_response_time_ms += response_time_ms;
        if (response_time_ms > perf_stats.max_response_time_ms) {
            perf_stats.max_response_time_ms = response_time_ms;
        }

        switch(event->type) {
            case MSG_SENSOR: stats.sensor_count++; break;
            case MSG_USER: stats.user_count++; break;
            case MSG_NETWORK: stats.network_count++; break;
            case MSG_TIMER: stats.timer_count++; break;
            case MSG_SPECIAL: stats.special_count++; break;
        }
        xSemaphoreGive(xStatsMutex);
    }

    // --- Processing Logic ---
    switch(event->type) {
        case MSG_SENSOR:
            ESP_LOGI(TAG, "→ Worker %d (Sensor): T=%.1f°C, H=%.1f%% (Resp: %lums)", 
                     worker_id, event->data.sensor_data.temperature, event->data.sensor_data.humidity, response_time_ms);
            if (event->data.sensor_data.temperature > 35.0) {
                ESP_LOGW(TAG, "⚠️   Worker %d: High temperature alert!", worker_id);
            }
            break;
        case MSG_USER:
            ESP_LOGI(TAG, "→ Worker %d (User): Button %d (%dms) (Resp: %lums)", 
                     worker_id, event->data.user_input.button_id, event->data.user_input.duration_ms, response_time_ms);
            break;
        case MSG_NETWORK:
            ESP_LOGI(TAG, "→ Worker %d (Network): [%s] %s (Resp: %lums)", 
                     worker_id, event->data.network_msg.source, event->data.network_msg.message, response_time_ms);
            if (event->data.network_msg.priority >= 4) {
                ESP_LOGW(TAG, "🚨 Worker %d: High priority network message!", worker_id);
            }
            break;
        case MSG_TIMER:
            ESP_LOGI(TAG, "→ Worker %d (Timer): Periodic maintenance (Resp: %lums)", worker_id, response_time_ms);
            break;
        case MSG_SPECIAL:
            ESP_LOGW(TAG, "!!! Worker %d (Special): Processed dynamic event ID %d (Resp: %lums) !!!", 
                     worker_id, event->data.special_event_id, response_time_ms);
            break;
    }

    // Simulate processing time
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_PROCESSOR, 0);
}

// --- NEW --- Challenge 3: Worker Task
void worker_task(void *pvParameters) {
    int worker_id = *((int*)pvParameters);
    event_message_t received_event;
    ESP_LOGI(TAG, "Worker task %d started", worker_id);

    while (1) {
        // --- NEW --- Challenge 1: Priority Handling
        // First, try to get a high-priority job (0 timeout)
        if (xQueueReceive(xHighPrioWorkerQueue, &received_event, 0) == pdPASS) {
            process_event(&received_event, worker_id);
        }
        // If no high-prio job, block and wait for a low-priority job
        else if (xQueueReceive(xLowPrioWorkerQueue, &received_event, portMAX_DELAY) == pdPASS) {
            process_event(&received_event, worker_id);
        }
        // (If both fail, loop continues and tries high-prio again)
    }
}

// --- MODIFIED --- 'processor_task' is now 'event_dispatcher_task'
void event_dispatcher_task(void *pvParameters) {
    QueueSetMemberHandle_t xActivatedMember;
    sensor_data_t sensor_data;
    user_input_t user_input;
    network_message_t network_msg;
    
    event_message_t event_to_dispatch;
    bool should_dispatch = false;
    
    // --- NEW --- Challenge 4: State for filtering
    float last_temp = -99.0;
    float last_humidity = -99.0;

    ESP_LOGI(TAG, "Event DISPATCHER task started - waiting for events...");
    
    while (1) {
        should_dispatch = false;

        // Wait for any event, but lock the set in case it's modified
        if (xSemaphoreTake(xQueueSetMutex, portMAX_DELAY) == pdTRUE) {
            xActivatedMember = xQueueSelectFromSet(xQueueSet, portMAX_DELAY);
            xSemaphoreGive(xQueueSetMutex);
        } else {
            continue; // Failed to take mutex? Should not happen.
        }
        
        // Determine which queue/semaphore was activated
        if (xActivatedMember == xSensorQueue) {
            if (xQueueReceive(xSensorQueue, &sensor_data, 0) == pdPASS) {
                // --- NEW --- Challenge 4: Event Filtering
                if (fabs(sensor_data.temperature - last_temp) < 1.0 && 
                    fabs(sensor_data.humidity - last_humidity) < 2.0) {
                    
                    ESP_LOGI(TAG, "↓ Dispatcher: Filtering out redundant sensor data.");
                    if(xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        stats.filtered_out++;
                        xSemaphoreGive(xStatsMutex);
                    }
                } else {
                    last_temp = sensor_data.temperature;
                    last_humidity = sensor_data.humidity;
                    event_to_dispatch.type = MSG_SENSOR;
                    event_to_dispatch.timestamp = sensor_data.timestamp;
                    event_to_dispatch.data.sensor_data = sensor_data;
                    should_dispatch = true;
                }
            }
        }
        else if (xActivatedMember == xUserQueue) {
            if (xQueueReceive(xUserQueue, &user_input, 0) == pdPASS) {
                event_to_dispatch.type = MSG_USER;
                event_to_dispatch.timestamp = user_input.timestamp;
                event_to_dispatch.data.user_input = user_input;
                should_dispatch = true;
            }
        }
        else if (xActivatedMember == xNetworkQueue) {
            if (xQueueReceive(xNetworkQueue, &network_msg, 0) == pdPASS) {
                event_to_dispatch.type = MSG_NETWORK;
                event_to_dispatch.timestamp = network_msg.timestamp;
                event_to_dispatch.data.network_msg = network_msg;
                should_dispatch = true;
            }
        }
        else if (xActivatedMember == xTimerSemaphore) {
            if (xSemaphoreTake(xTimerSemaphore, 0) == pdPASS) {
                event_to_dispatch.type = MSG_TIMER;
                event_to_dispatch.timestamp = xTaskGetTickCount(); // Timer needs a timestamp too
                should_dispatch = true;
            }
        }
        // --- NEW --- Challenge 2: Handle dynamically added queue
        else if (xActivatedMember != NULL && xActivatedMember == xSpecialEventQueue) {
            int special_id;
            if (xQueueReceive(xSpecialEventQueue, &special_id, 0) == pdPASS) {
                event_to_dispatch.type = MSG_SPECIAL;
                event_to_dispatch.timestamp = xTaskGetTickCount();
                event_to_dispatch.data.special_event_id = special_id;
                should_dispatch = true;
                ESP_LOGW(TAG, "!!! Dispatcher: Got special event %d !!!", special_id);
            }
        }
        
        // --- MODIFIED --- Route to correct priority queue
        if (should_dispatch) {
            // --- NEW --- Challenge 1: Priority Handling
            if (event_to_dispatch.type == MSG_USER || 
                event_to_dispatch.type == MSG_NETWORK || 
                event_to_dispatch.type == MSG_SPECIAL) 
            {
                // Send to HIGH priority queue (don't block)
                if(xQueueSend(xHighPrioWorkerQueue, &event_to_dispatch, 0) != pdPASS) {
                    ESP_LOGE(TAG, "!!! HIGH Prio Worker Queue FULL !!!");
                }
            } else {
                // Send to LOW priority queue (don't block)
                if(xQueueSend(xLowPrioWorkerQueue, &event_to_dispatch, 0) != pdPASS) {
                    ESP_LOGE(TAG, "!!! LOW Prio Worker Queue FULL !!!");
                }
            }
        }
    }
}

// --- MODIFIED --- System monitor task
void monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "System monitor started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        
        ESP_LOGI(TAG, "\n════════════ SYSTEM MONITOR ════════════");
        
        // Get queue stats
        UBaseType_t sensor_q = uxQueueMessagesWaiting(xSensorQueue);
        UBaseType_t user_q = uxQueueMessagesWaiting(xUserQueue);
        UBaseType_t net_q = uxQueueMessagesWaiting(xNetworkQueue);
        UBaseType_t high_prio_q = uxQueueMessagesWaiting(xHighPrioWorkerQueue);
        UBaseType_t low_prio_q = uxQueueMessagesWaiting(xLowPrioWorkerQueue);
        
        ESP_LOGI(TAG, "--- Source Queues (Backlog) ---");
        ESP_LOGI(TAG, "  Sensor Queue:  %d/%d", sensor_q, 5);
        ESP_LOGI(TAG, "  User Queue:    %d/%d", user_q, 3);
        ESP_LOGI(TAG, "  Network Queue: %d/%d", net_q, 8);
        
        ESP_LOGI(TAG, "--- Worker Queues (Load) ---");
        ESP_LOGI(TAG, "  HIGH Prio Worker Q: %d/%d", high_prio_q, HIGH_PRIO_QUEUE_LEN);
        ESP_LOGI(TAG, "  LOW Prio Worker Q:  %d/%d", low_prio_q, LOW_PRIO_QUEUE_LEN);
        
        // Get message and performance stats safely
        if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint32_t avg_response_ms = 0;
            if (perf_stats.total_events_processed > 0) {
                avg_response_ms = (uint32_t)(perf_stats.total_response_time_ms / perf_stats.total_events_processed);
            }
            
            ESP_LOGI(TAG, "--- Message Statistics (Processed) ---");
            ESP_LOGI(TAG, "  Sensor:  %lu", stats.sensor_count);
            ESP_LOGI(TAG, "  User:    %lu", stats.user_count);
            ESP_LOGI(TAG, "  Network: %lu", stats.network_count);
            ESP_LOGI(TAG, "  Timer:   %lu", stats.timer_count);
            ESP_LOGI(TAG, "  Special: %lu", stats.special_count);
            ESP_LOGI(TAG, "  Filtered: %lu", stats.filtered_out);
            
            ESP_LOGI(TAG, "--- Performance Metrics (Challenge 5) ---");
            ESP_LOGI(TAG, "  Avg. Response Time: %lu ms", avg_response_ms);
            ESP_LOGI(TAG, "  Max. Response Time: %lu ms", perf_stats.max_response_time_ms);
            ESP_LOGI(TAG, "  Total Events:       %lu", perf_stats.total_events_processed);

            xSemaphoreGive(xStatsMutex);
        } else {
            ESP_LOGE(TAG, "Monitor task failed to get stats mutex!");
        }
        ESP_LOGI(TAG, "════════════════════════════════════════\n");
    }
}

// --- MODIFIED --- app_main
void app_main(void) {
    ESP_LOGI(TAG, "Advanced Event Bus Lab Starting...");
    
    // Configure LEDs (no change)
    gpio_set_direction(LED_SENSOR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_USER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_NETWORK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TIMER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PROCESSOR, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SENSOR, 0);
    gpio_set_level(LED_USER, 0);
    gpio_set_level(LED_NETWORK, 0);
    gpio_set_level(LED_TIMER, 0);
    gpio_set_level(LED_PROCESSOR, 0);
    
    // --- NEW --- Create Mutexes
    xStatsMutex = xSemaphoreCreateMutex();
    xQueueSetMutex = xSemaphoreCreateMutex();

    // Create individual source queues
    xSensorQueue = xQueueCreate(5, sizeof(sensor_data_t));
    xUserQueue = xQueueCreate(3, sizeof(user_input_t));
    xNetworkQueue = xQueueCreate(8, sizeof(network_message_t));
    xTimerSemaphore = xSemaphoreCreateBinary();

    // --- NEW --- Create worker queues
    xHighPrioWorkerQueue = xQueueCreate(HIGH_PRIO_QUEUE_LEN, sizeof(event_message_t));
    xLowPrioWorkerQueue = xQueueCreate(LOW_PRIO_QUEUE_LEN, sizeof(event_message_t));
    
    // Create queue set (size = 5+3+8+1 + 1 for the special queue)
    xQueueSet = xQueueCreateSet(5 + 3 + 8 + 1 + 2); 
    
    if (xSensorQueue && xUserQueue && xNetworkQueue && xTimerSemaphore && 
        xQueueSet && xStatsMutex && xQueueSetMutex && 
        xHighPrioWorkerQueue && xLowPrioWorkerQueue) {
        
        // Add *initial* queues to the set
        if (xQueueAddToSet(xSensorQueue, xQueueSet) != pdPASS ||
            xQueueAddToSet(xUserQueue, xQueueSet) != pdPASS ||
            xQueueAddToSet(xNetworkQueue, xQueueSet) != pdPASS ||
            xQueueAddToSet(xTimerSemaphore, xQueueSet) != pdPASS) {
            
            ESP_LOGE(TAG, "Failed to add initial queues to queue set!");
            return;
        }
        
        ESP_LOGI(TAG, "All queues, mutexes, and set created successfully");
        
        // Create producer tasks
        xTaskCreate(sensor_task, "Sensor", 2048, NULL, 3, NULL);
        xTaskCreate(user_input_task, "UserInput", 2048, NULL, 3, NULL);
        xTaskCreate(network_task, "Network", 2048, NULL, 3, NULL);
        xTaskCreate(timer_task, "Timer", 2048, NULL, 2, NULL);
        
        // --- MODIFIED --- Create dispatcher task
        xTaskCreate(event_dispatcher_task, "Dispatcher", 3072, NULL, 4, NULL);
        
        // --- NEW --- Create Worker Tasks (Challenge 3)
        static int worker_ids[NUM_WORKERS];
        for (int i = 0; i < NUM_WORKERS; i++) {
            worker_ids[i] = i;
            char task_name[20];
            snprintf(task_name, sizeof(task_name), "Worker%d", i);
            xTaskCreate(worker_task, task_name, 3072, &worker_ids[i], 3, NULL);
        }

        // Create monitor task
        xTaskCreate(monitor_task, "Monitor", 3072, NULL, 1, NULL);

        // --- NEW --- Create dynamic queue task (Challenge 2)
        xTaskCreate(special_event_task, "SpecialEvent", 2048, NULL, 1, NULL);
        
        ESP_LOGI(TAG, "All tasks created. System operational.");
        
        // (Removed startup LED sequence to start faster)
        
    } else {
        ESP_LOGE(TAG, "Failed to create one or more system components!");
    }
}
