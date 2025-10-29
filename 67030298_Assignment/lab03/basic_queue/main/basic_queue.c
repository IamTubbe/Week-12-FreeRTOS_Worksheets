#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h" // --- NEW --- Added for mutex
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "QUEUE_LAB_ADVANCED";

// LED pins
#define LED_SENDER GPIO_NUM_2
#define LED_RECEIVER GPIO_NUM_4

// --- NEW --- Challenge 4: Dynamic Queue Size
#define HIGH_PRIO_QUEUE_LENGTH 5
#define LOW_PRIO_QUEUE_LENGTH 5

// --- MODIFIED --- Challenge 1: Priority Queue (using 2 queues)
QueueHandle_t xQueueHighPriority;
QueueHandle_t xQueueLowPriority;

// --- NEW --- Challenge 3: Queue Statistics
static SemaphoreHandle_t stats_mutex;
static uint32_t dropped_messages = 0;

// --- MODIFIED --- Data structure (added priority field)
typedef struct {
    int id;
    int priority; // 0 = Low, 1 = High
    char message[50];
    uint32_t timestamp;
} queue_message_t;

// --- NEW --- Challenge 2: Multiple Senders (Parameter struct)
typedef struct {
    const char *name;
    QueueHandle_t queue_to_send;
    int priority;
    TickType_t rate_delay_ms;
} sender_params_t;


// --- MODIFIED --- Sender task now accepts parameters
void sender_task(void *pvParameters) {
    sender_params_t *params = (sender_params_t *)pvParameters; // --- MODIFIED ---
    queue_message_t message;
    int counter = 0;
    
    ESP_LOGI(TAG, "%s task started (Rate: %lu ms, Prio: %d)", 
             params->name, params->rate_delay_ms, params->priority);
    
    while (1) {
        // Prepare message
        message.id = counter++;
        message.priority = params->priority; // --- MODIFIED ---
        snprintf(message.message, sizeof(message.message), 
                 "Hello from %s #%d", params->name, message.id);
        message.timestamp = xTaskGetTickCount();
        
        // Send message to its assigned queue
        // We use a 100ms timeout instead of 1000ms to fail faster
        BaseType_t xStatus = xQueueSend(params->queue_to_send, &message, pdMS_TO_TICKS(100)); // --- MODIFIED ---
        
        if (xStatus == pdPASS) {
            ESP_LOGI(TAG, "%s Sent: ID=%d, PRIO=%d", 
                     params->name, message.id, message.priority);
            
            // Blink sender LED (shared by both senders)
            gpio_set_level(LED_SENDER, 1);
            vTaskDelay(pdMS_TO_TICKS(100)); // Shorter blink
            gpio_set_level(LED_SENDER, 0);
        } else {
            ESP_LOGW(TAG, "%s: Failed to send message (queue full?)", params->name);
            
            // --- NEW --- Challenge 3: Increment dropped message counter
            if (xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                dropped_messages++;
                xSemaphoreGive(stats_mutex);
            }
        }
        
        // Wait based on the task's defined rate
        vTaskDelay(pdMS_TO_TICKS(params->rate_delay_ms)); // --- MODIFIED ---
    }
}

// --- NEW --- Helper function to process received messages
void process_message(queue_message_t *msg, const char *prio_str) {
    ESP_LOGI(TAG, "Received [%s]: ID=%d, PRIO=%d, MSG=%s, Time=%lu", 
             prio_str, msg->id, msg->priority, msg->message, msg->timestamp);
             
    // Blink receiver LED
    gpio_set_level(LED_RECEIVER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_RECEIVER, 0);
    
    // Process time (simulate work)
    // --- MODIFIED --- Increased simulated work time to 1 second
    // This will make the queues fill up, testing the drop logic.
    vTaskDelay(pdMS_TO_TICKS(1000)); 
}

// --- MODIFIED --- Receiver task now implements priority logic
void receiver_task(void *pvParameters) {
    queue_message_t received_message;
    BaseType_t xStatus;
    
    ESP_LOGI(TAG, "Receiver task started (Processing time: 1000ms)");
    
    while (1) {
        // --- MODIFIED --- Challenge 1: Priority Queue Logic
        
        // 1. Try to receive from the HIGH priority queue first.
        // Use a 0-tick timeout (don't block, just check if data is available)
        xStatus = xQueueReceive(xQueueHighPriority, &received_message, 0);
        
        if (xStatus == pdPASS) {
            // Got a high-priority message
            process_message(&received_message, "HIGH PRIO");
        } else {
            // 2. If HIGH priority queue was empty, block and wait on the LOW priority queue.
            // We use the original 5-second timeout here.
            xStatus = xQueueReceive(xQueueLowPriority, &received_message, pdMS_TO_TICKS(5000));
            
            if (xStatus == pdPASS) {
                // Got a low-priority message
                process_message(&received_message, "LOW PRIO");
            } else {
                // Both queues were empty for the timeout duration
                ESP_LOGW(TAG, "No message received on any queue within timeout");
            }
        }
    }
}

// --- NEW --- Helper function to print queue visual
void print_queue_visual(const char *name, UBaseType_t messages, UBaseType_t length) {
    printf("%-10s Queue: [", name); // %-10s ensures alignment
    for (int i = 0; i < length; i++) {
        printf(i < messages ? "■" : "□");
    }
    printf("] (%d / %d)\n", messages, length);
}

// --- MODIFIED --- Queue monitoring task now monitors both queues and dropped stats
void queue_monitor_task(void *pvParameters) {
    UBaseType_t highPrioMessages, highPrioSpaces;
    UBaseType_t lowPrioMessages, lowPrioSpaces;
    uint32_t current_dropped = 0;
    
    ESP_LOGI(TAG, "Queue monitor task started");
    
    while (1) {
        // Get stats for both queues
        highPrioMessages = uxQueueMessagesWaiting(xQueueHighPriority);
        highPrioSpaces = uxQueueSpacesAvailable(xQueueHighPriority);
        lowPrioMessages = uxQueueMessagesWaiting(xQueueLowPriority);
        lowPrioSpaces = uxQueueSpacesAvailable(xQueueLowPriority);
        
        // --- NEW --- Get dropped message count safely
        if (xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            current_dropped = dropped_messages;
            xSemaphoreGive(stats_mutex);
        }
        
        // Log stats
        ESP_LOGI(TAG, "--- Queue Status ---");
        ESP_LOGI(TAG, "High Prio - Messages: %d, Free: %d", highPrioMessages, highPrioSpaces);
        ESP_LOGI(TAG, "Low Prio  - Messages: %d, Free: %d", lowPrioMessages, lowPrioSpaces);
        ESP_LOGI(TAG, "Statistics - Dropped Messages: %lu", current_dropped);
        
        // --- MODIFIED --- Show visual for both queues
        print_queue_visual("High Prio", highPrioMessages, HIGH_PRIO_QUEUE_LENGTH);
        print_queue_visual("Low Prio", lowPrioMessages, LOW_PRIO_QUEUE_LENGTH);
        printf("\n"); // Add a newline for readability
        
        vTaskDelay(pdMS_TO_TICKS(3000)); // Monitor every 3 seconds
    }
}

// --- NEW --- Define sender parameters
static sender_params_t sender_high_prio_params = {
    .name = "Sender High",
    .queue_to_send = NULL, // Will be set in app_main
    .priority = 1,
    .rate_delay_ms = 500   // Fast sender
};

static sender_params_t sender_low_prio_params = {
    .name = "Sender Low",
    .queue_to_send = NULL, // Will be set in app_main
    .priority = 0,
    .rate_delay_ms = 1500  // Slow sender
};


void app_main(void) {
    ESP_LOGI(TAG, "Advanced Queue Operations Lab Starting...");
    
    // Configure LED pins
    gpio_set_direction(LED_SENDER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RECEIVER, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SENDER, 0);
    gpio_set_level(LED_RECEIVER, 0);
    
    // --- NEW --- Create mutex for stats
    stats_mutex = xSemaphoreCreateMutex();
    
    // --- MODIFIED --- Create two queues
    xQueueHighPriority = xQueueCreate(HIGH_PRIO_QUEUE_LENGTH, sizeof(queue_message_t));
    xQueueLowPriority = xQueueCreate(LOW_PRIO_QUEUE_LENGTH, sizeof(queue_message_t));
    
    // --- NEW --- Assign queue handles to sender params
    sender_high_prio_params.queue_to_send = xQueueHighPriority;
    sender_low_prio_params.queue_to_send = xQueueLowPriority;

    if (xQueueHighPriority != NULL && xQueueLowPriority != NULL && stats_mutex != NULL) {
        ESP_LOGI(TAG, "Queues and Mutex created successfully");
        
        // --- MODIFIED --- Create tasks
        // Challenge 2: Create two sender tasks with different parameters
        xTaskCreate(sender_task, "Sender High", 2048, &sender_high_prio_params, 2, NULL);
        xTaskCreate(sender_task, "Sender Low", 2048, &sender_low_prio_params, 2, NULL);
        
        xTaskCreate(receiver_task, "Receiver", 2048, NULL, 1, NULL);
        xTaskCreate(queue_monitor_task, "Monitor", 2048, NULL, 1, NULL);
        
        ESP_LOGI(TAG, "All tasks created. Starting scheduler...");
    } else {
        ESP_LOGE(TAG, "Failed to create queues or mutex!");
    }
}
