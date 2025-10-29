#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // Now includes Queue
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_random.h"
#include "esp_timer.h" // For high-resolution timer (Challenge 4)

static const char *TAG = "ADV_LAB";

// LED pins
#define LED_PRODUCER GPIO_NUM_2
#define LED_CONSUMER_1 GPIO_NUM_4 // Specific LED for Consumer 1
#define LED_CONSUMER_2 GPIO_NUM_18 // Specific LED for Consumer 2 (NEW)
#define LED_TIMER GPIO_NUM_5
#define BUTTON_PIN GPIO_NUM_0

// --- CHALLENGE 4: Replace Binary Semaphore with a Queue ---
// We use a Queue to pass data (the timestamp) for latency measurement
QueueHandle_t xEventDataQueue;

// --- CHALLENGE 1 & 5: Add Mutex for stats ---
SemaphoreHandle_t xStatsMutex;

// Semaphore handles for ISRs
SemaphoreHandle_t xTimerSemaphore;
SemaphoreHandle_t xButtonSemaphore;

// Timer handle
gptimer_handle_t gptimer = NULL;

// Statistics (Updated for Challenges 3 & 5)
typedef struct {
    uint32_t signals_sent;
    uint32_t signals_received;
    uint32_t timer_events;
    uint32_t button_presses;
    uint32_t timeouts;         // CHALLENGE 3: Track timeouts
    uint32_t queue_send_fails; // CHALLENGE 5: Track errors
} semaphore_stats_t;

semaphore_stats_t stats = {0, 0, 0, 0, 0, 0};

// Timer callback function (ISR context)
static bool IRAM_ATTR timer_callback(gptimer_handle_t timer, 
                                     const gptimer_alarm_event_data_t *edata, 
                                     void *user_data) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xTimerSemaphore, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}

// Button interrupt handler (ISR context)
static void IRAM_ATTR button_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xButtonSemaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// producer_task
void producer_task(void *pvParameters) {
    int event_counter = 0;
    ESP_LOGI(TAG, "Producer task started (Priority 2)");
    
    while (1) {
        // 1. Simulate work
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000))); // 2-5 seconds
        
        event_counter++;
        ESP_LOGI(TAG, "🔥 Producer: Generating event batch #%d", event_counter);

        // 2. Try to give 3 times (will now likely succeed thanks to Queue)
        for (int i = 0; i < 3; i++) {
            ESP_LOGI(TAG, "  -> Attempting to send signal %d/3...", i + 1);
            
            // CHALLENGE 4: Send timestamp to queue
            uint64_t timestamp = esp_timer_get_time();
            
            // Send with a 0-tick timeout (fail immediately if full)
            if (xQueueSend(xEventDataQueue, &timestamp, 0) == pdTRUE) {
                
                // CHALLENGE 5: Thread-safe stats update
                xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                stats.signals_sent++;
                xSemaphoreGive(xStatsMutex);
                
                ESP_LOGI(TAG, "  ✓  Producer: Signal %d/3 sent successfully", i + 1);
                
                gpio_set_level(LED_PRODUCER, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(LED_PRODUCER, 0);
                
            } else {
                // CHALLENGE 5: Log error
                xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                stats.queue_send_fails++;
                xSemaphoreGive(xStatsMutex);
                ESP_LOGW(TAG, "  ✗  Producer: Failed to send signal %d/3 (Queue full!)");
            }
            
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
    }
}

// CHALLENGE 1: Consumer task modified to accept an ID
void consumer_task(void *pvParameters) {
    int consumer_id = (int)pvParameters;
    uint8_t led_pin = (consumer_id == 1) ? LED_CONSUMER_1 : LED_CONSUMER_2;
    char log_tag[20];
    sprintf(log_tag, "Consumer %d", consumer_id);

    ESP_LOGI(log_tag, "Task started - waiting for events... (Priority 3)");
    
    uint64_t timestamp_sent; // For Challenge 4

    while (1) {
        ESP_LOGI(log_tag, "🔍 Waiting for event...");
        
        // CHALLENGE 4: Receive from Queue instead of taking semaphore
        if (xQueueReceive(xEventDataQueue, &timestamp_sent, pdMS_TO_TICKS(3000)) == pdTRUE) {
            
            // CHALLENGE 4: Performance Analysis
            uint64_t timestamp_received = esp_timer_get_time();
            uint32_t latency_us = (uint32_t)(timestamp_received - timestamp_sent);

            // CHALLENGE 5: Thread-safe stats update
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.signals_received++;
            xSemaphoreGive(xStatsMutex);
            
            ESP_LOGI(log_tag, "⚡ Event received! Latency: %lu us. Processing...", latency_us);
            
            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000))); // 1-3 seconds
            gpio_set_level(led_pin, 0);
            
            ESP_LOGI(log_tag, "✓ Event processed successfully");
            
        } else {
            // CHALLENGE 3: Timeout Handling
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.timeouts++;
            xSemaphoreGive(xStatsMutex);
            ESP_LOGW(log_tag, "⏰ Timeout waiting for event");
        }
    }
}

// Timer event handler task
void timer_event_task(void *pvParameters) {
    ESP_LOGI(TAG, "Timer event task started (Priority 2)");
    
    while (1) {
        if (xSemaphoreTake(xTimerSemaphore, portMAX_DELAY) == pdTRUE) {
            
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.timer_events++;
            uint32_t current_timer_events = stats.timer_events;
            xSemaphoreGive(xStatsMutex);

            ESP_LOGI(TAG, "⏱️  Timer: Periodic timer event #%lu", current_timer_events);
            
            gpio_set_level(LED_TIMER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_TIMER, 0);
            
            // Show statistics every 5 timer events
            if (current_timer_events % 5 == 0) {
                // Copy stats inside mutex for thread-safe logging
                xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                semaphore_stats_t current_stats = stats;
                xSemaphoreGive(xStatsMutex);

                ESP_LOGI(TAG, "📊 Stats - Sent:%lu, Received:%lu, Timeouts:%lu, Fails:%lu, Timer:%lu, Button:%lu", 
                         current_stats.signals_sent, current_stats.signals_received, 
                         current_stats.timeouts, current_stats.queue_send_fails,
                         current_stats.timer_events, current_stats.button_presses);
            }
        }
    }
}

// Button event handler task
void button_event_task(void *pvParameters) {
    ESP_LOGI(TAG, "Button event task started (Priority 4)");
    
    while (1) {
        if (xSemaphoreTake(xButtonSemaphore, portMAX_DELAY) == pdTRUE) {
            
            xSemaphoreTake(xStatsMutex, portMAX_DELAY);
            stats.button_presses++;
            uint32_t press_count = stats.button_presses;
            xSemaphoreGive(xStatsMutex);
            
            ESP_LOGI(TAG, "🔘 Button: Press detected #%lu", press_count);
            
            // Debounce delay
            vTaskDelay(pdMS_TO_TICKS(300));
            
            ESP_LOGI(TAG, "🚀 Button: Triggering immediate producer event");
            
            // CHALLENGE 4: Send timestamp
            uint64_t timestamp = esp_timer_get_time();
            if (xQueueSend(xEventDataQueue, &timestamp, 0) == pdTRUE) {
                xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                stats.signals_sent++;
                xSemaphoreGive(xStatsMutex);
            } else {
                // CHALLENGE 5
                xSemaphoreTake(xStatsMutex, portMAX_DELAY);
                stats.queue_send_fails++;
                xSemaphoreGive(xStatsMutex);
                ESP_LOGW(TAG, "🚀 Button: Failed to send event (Queue full!)");
            }
        }
    }
}

// System monitor task
void monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "System monitor started (Priority 1)");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); // Every 15 seconds
        
        // CHALLENGE 5: Must use mutex to read stats
        xSemaphoreTake(xStatsMutex, portMAX_DELAY);
        semaphore_stats_t current_stats = stats;
        // Check queue status while we have the mutex (though not strictly needed)
        UBaseType_t items_in_queue = uxQueueMessagesWaiting(xEventDataQueue);
        xSemaphoreGive(xStatsMutex);
        
        ESP_LOGI(TAG, "\n═══ SEMAPHORE SYSTEM MONITOR ═══");
        
        // CHALLENGE 4: Report Queue status
        ESP_LOGI(TAG, "Event Data Queue: %lu / 5 items", items_in_queue);
        
        ESP_LOGI(TAG, "Timer Semaphore Count: %d", uxSemaphoreGetCount(xTimerSemaphore));
        ESP_LOGI(TAG, "Button Semaphore Count: %d", uxSemaphoreGetCount(xButtonSemaphore));
        
        ESP_LOGI(TAG, "Event Statistics:");
        ESP_LOGI(TAG, "  Producer Events: %lu", current_stats.signals_sent);
        ESP_LOGI(TAG, "  Consumer Events: %lu", current_stats.signals_received);
        ESP_LOGI(TAG, "  Consumer Timeouts: %lu", current_stats.timeouts); // CHALLENGE 3
        ESP_LOGI(TAG, "  Queue Send Fails: %lu", current_stats.queue_send_fails); // CHALLENGE 5
        ESP_LOGI(TAG, "  Timer Events:     %lu", current_stats.timer_events);
        ESP_LOGI(TAG, "  Button Presses:   %lu", current_stats.button_presses);
        
        float efficiency = current_stats.signals_sent > 0 ? 
                           (float)current_stats.signals_received / current_stats.signals_sent * 100 : 0;
        ESP_LOGI(TAG, "  System Efficiency: %.1f%%", efficiency);
        ESP_LOGI(TAG, "══════════════════════════════\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Advanced RTOS Lab Starting...");
    
    // Configure LED pins
    gpio_set_direction(LED_PRODUCER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_2, GPIO_MODE_OUTPUT); // NEW
    gpio_set_direction(LED_TIMER, GPIO_MODE_OUTPUT);
    
    // Configure button pin
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(BUTTON_PIN, GPIO_INTR_NEGEDGE);
    
    // Turn off all LEDs
    gpio_set_level(LED_PRODUCER, 0);
    gpio_set_level(LED_CONSUMER_1, 0);
    gpio_set_level(LED_CONSUMER_2, 0);
    gpio_set_level(LED_TIMER, 0);
    
    // Create RTOS objects
    // CHALLENGE 4: Create a queue of length 5, holding uint64_t items
    xEventDataQueue = xQueueCreate(5, sizeof(uint64_t)); 
    
    // CHALLENGE 5: Create mutex for stats
    xStatsMutex = xSemaphoreCreateMutex(); 
    
    xTimerSemaphore = xSemaphoreCreateBinary();
    xButtonSemaphore = xSemaphoreCreateBinary();
    
    if (xEventDataQueue && xTimerSemaphore && xButtonSemaphore && xStatsMutex) {
        ESP_LOGI(TAG, "All Queues, Semaphores, and Mutex created successfully");
        
        // Install GPIO ISR service
        gpio_install_isr_service(0);
        gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);
        
        // ... (Timer configuration remains the same) ...
        gptimer_config_t timer_config = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000, // 1MHz, 1 tick=1us
        };
        ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
        
        gptimer_event_callbacks_t cbs = {
            .on_alarm = timer_callback,
        };
        ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
        ESP_ERROR_CHECK(gptimer_enable(gptimer));
        
        gptimer_alarm_config_t alarm_config = {
            .alarm_count = 8000000, // 8 seconds
            .reload_count = 0,
            .flags.auto_reload_on_alarm = true,
        };
        ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));
        ESP_ERROR_CHECK(gptimer_start(gptimer));
        
        ESP_LOGI(TAG, "Timer configured for 8-second intervals");
        
        // --- CHALLENGE 1 & 2: Create tasks with new priorities ---
        // Button is highest, Consumers are next, Producer/Timer are lower
        xTaskCreate(producer_task, "Producer", 2048, NULL, 2, NULL); 
        xTaskCreate(timer_event_task, "TimerEvent", 2048, NULL, 2, NULL); 
        xTaskCreate(monitor_task, "Monitor", 2048, NULL, 1, NULL); // Lowest priority
        
        // Create TWO Consumer tasks
        xTaskCreate(consumer_task, "Consumer_1", 2048, (void*)1, 3, NULL); 
        xTaskCreate(consumer_task, "Consumer_2", 2048, (void*)2, 3, NULL); 
        
        xTaskCreate(button_event_task, "ButtonEvent", 2048, NULL, 4, NULL); // Highest priority
        
        ESP_LOGI(TAG, "All tasks created. System operational.");
        ESP_LOGI(TAG, "💡 Press the BOOT button (GPIO0) to trigger immediate events!");
        
    } else {
        ESP_LOGE(TAG, "Failed to create RTOS objects!");
    }
}
