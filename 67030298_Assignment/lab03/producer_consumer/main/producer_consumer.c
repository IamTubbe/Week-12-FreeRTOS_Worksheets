#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "SMART_FACTORY";

// LED pins
#define LED_PRODUCER_1 GPIO_NUM_2
#define LED_PRODUCER_2 GPIO_NUM_4
#define LED_PRODUCER_3 GPIO_NUM_5
#define LED_INSPECTOR  GPIO_NUM_18 // --- MODIFIED ---
#define LED_CONSUMER_1 GPIO_NUM_19 // --- MODIFIED ---

// --- NEW --- Challenge 2 & 3: Multiple Queues
#define QUEUE_A_LENGTH 10
#define QUEUE_B_LENGTH 5
#define FINISHED_GOODS_QUEUE_LENGTH 15
#define STATS_QUEUE_LENGTH 1 // For network task
#define CONSUMER_BATCH_SIZE 4 // --- NEW --- Challenge 4

// --- MODIFIED --- Queue Handles
QueueHandle_t xQueueProductionA;
QueueHandle_t xQueueProductionB;
QueueHandle_t xQueueFinishedGoods;
QueueHandle_t xStatsQueue; // --- NEW --- Challenge 5
QueueSetHandle_t xProductionQueueSet; // --- NEW --- Challenge 3
SemaphoreHandle_t xPrintMutex;

// --- MODIFIED --- Statistics
typedef struct {
    uint32_t produced;
    uint32_t consumed;
    uint32_t dropped;
    uint32_t failed_qc; // --- NEW --- Challenge 3
} stats_t;

stats_t global_stats = {0, 0, 0, 0};

// --- NEW --- Challenge 2: Product Categories
typedef enum {
    CATEGORY_A,
    CATEGORY_B
} product_category_t;

// --- NEW --- Challenge 3: QC Status
typedef enum {
    QC_PENDING,
    QC_PASS,
    QC_FAIL
} qc_status_t;

// --- MODIFIED --- Product data structure
typedef struct {
    int producer_id;
    int product_id;
    product_category_t category; // --- NEW ---
    qc_status_t qc_status;       // --- NEW ---
    char product_name[30];
    uint32_t production_time;
    int processing_time_ms; // This is now "CONSUMER" processing time
} product_t;

// Safe printing function (no change)
void safe_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (xSemaphoreTake(xPrintMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        vprintf(format, args);
        xSemaphoreGive(xPrintMutex);
    }
    va_end(args);
}

// --- MODIFIED --- Producer task
void producer_task(void *pvParameters) {
    int producer_id = *((int*)pvParameters);
    product_t product;
    int product_counter = 0;
    gpio_num_t led_pin;
    
    switch (producer_id) {
        case 1: led_pin = LED_PRODUCER_1; break;
        case 2: led_pin = LED_PRODUCER_2; break;
        case 3: led_pin = LED_PRODUCER_3; break;
        default: led_pin = LED_PRODUCER_1;
    }
    
    safe_printf("Producer %d started\n", producer_id);
    
    while (1) {
        // Create product
        product.producer_id = producer_id;
        product.product_id = product_counter++;
        product.qc_status = QC_PENDING;
        product.production_time = xTaskGetTickCount();
        product.processing_time_ms = 200 + (esp_random() % 800); // 0.2-1.0 sec consumer time

        // --- NEW --- Challenge 2: Assign random category
        QueueHandle_t targetQueue;
        char cat_str[3];
        if (esp_random() % 2 == 0) {
            product.category = CATEGORY_A;
            targetQueue = xQueueProductionA;
            strcpy(cat_str, "A");
        } else {
            product.category = CATEGORY_B;
            targetQueue = xQueueProductionB;
            strcpy(cat_str, "B");
        }
        
        snprintf(product.product_name, sizeof(product.product_name), 
                 "Product-%s-P%d-#%d", cat_str, producer_id, product.product_id);
        
        // --- MODIFIED --- Try to send product to the correct category queue
        BaseType_t xStatus = xQueueSend(targetQueue, &product, pdMS_TO_TICKS(100));
        
        if (xStatus == pdPASS) {
            global_stats.produced++;
            safe_printf("✓ Producer %d: Created %s\n", producer_id, product.product_name);
            
            // Blink producer LED
            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(led_pin, 0);
        } else {
            global_stats.dropped++;
            safe_printf("✗ Producer %d: Queue full! Dropped %s\n", producer_id, product.product_name);
        }
        
        int delay = 1000 + (esp_random() % 2000);
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

// --- NEW --- Challenge 3: Quality Control (Inspector) Task
void inspector_task(void *pvParameters) {
    QueueSetMemberHandle_t xActivatedQueue;
    product_t product;
    safe_printf("Inspector task started (Monitoring Queues A & B)\n");

    while(1) {
        // Wait on the Queue Set to know which queue has data
        xActivatedQueue = xQueueSelectFromSet(xProductionQueueSet, pdMS_TO_TICKS(5000));

        if (xActivatedQueue == NULL) {
            safe_printf("⏰ Inspector: No products to inspect (timeout)\n");
            continue;
        }

        // Receive from the queue that was activated
        if (xQueueReceive(xActivatedQueue, &product, 0) != pdPASS) {
            // This should not happen if SelectFromSet was successful, but good to check
            continue;
        }
        
        safe_printf("→ Inspector: Checking %s...\n", product.product_name);

        // Turn on inspector LED during processing
        gpio_set_level(LED_INSPECTOR, 1);
        // Simulate QC time (0.3 - 1.0 seconds)
        vTaskDelay(pdMS_TO_TICKS(300 + (esp_random() % 700)));
        gpio_set_level(LED_INSPECTOR, 0);

        // Randomly pass (90%) or fail (10%)
        if (esp_random() % 100 < 90) {
            product.qc_status = QC_PASS;
            safe_printf("✓ Inspector: %s PASSED QC. Sent to Finished Goods.\n", product.product_name);
            
            // Send to the next stage
            if (xQueueSend(xQueueFinishedGoods, &product, pdMS_TO_TICKS(100)) != pdPASS) {
                safe_printf("✗ Inspector: Finished Goods Queue FULL! Dropped %s\n", product.product_name);
                // Note: This could be a new "dropped" category, but we'll re-use global_stats.dropped
                global_stats.dropped++;
            }
        } else {
            product.qc_status = QC_FAIL;
            global_stats.failed_qc++;
            safe_printf("✗ Inspector: %s FAILED QC! Discarded.\n", product.product_name);
        }
    }
}


// --- MODIFIED --- Challenge 4: Batch Processing Consumer
void consumer_task(void *pvParameters) {
    int consumer_id = *((int*)pvParameters);
    product_t batch[CONSUMER_BATCH_SIZE];
    int batch_count = 0;
    int total_processing_time = 0;
    
    safe_printf("Consumer %d started (Batch Size: %d)\n", consumer_id, CONSUMER_BATCH_SIZE);
    
    while (1) {
        batch_count = 0;
        total_processing_time = 0;

        // 1. Wait for the FIRST item in the batch (long timeout)
        BaseType_t xStatus = xQueueReceive(xQueueFinishedGoods, &batch[0], pdMS_TO_TICKS(5000));

        if (xStatus == pdPASS) {
            // Got one item, start the batch
            batch_count = 1;
            total_processing_time += batch[0].processing_time_ms;

            // 2. Try to fill the REST of the batch (zero timeout)
            for (int i = 1; i < CONSUMER_BATCH_SIZE; i++) {
                if (xQueueReceive(xQueueFinishedGoods, &batch[i], 0) == pdPASS) {
                    // Got another item
                    batch_count++;
                    total_processing_time += batch[i].processing_time_ms;
                } else {
                    // Queue is empty, stop filling batch
                    break;
                }
            }
        }

        // 3. Process the entire batch if we have any items
        if (batch_count > 0) {
            safe_printf("→ Consumer %d: Processing BATCH of %d items (total time: %dms)\n", 
                        consumer_id, batch_count, total_processing_time);
            
            // Turn on consumer LED during *entire* batch processing
            gpio_set_level(LED_CONSUMER_1, 1);
            vTaskDelay(pdMS_TO_TICKS(total_processing_time));
            gpio_set_level(LED_CONSUMER_1, 0);
            
            // Update stats *after* batch is processed
            global_stats.consumed += batch_count;
            
            safe_printf("✓ Consumer %d: Finished BATCH of %d items.\n", consumer_id, batch_count);
        } else {
            // Timed out waiting for the first item
            safe_printf("⏰ Consumer %d: No products to process (timeout)\n", consumer_id);
        }
    }
}

// --- NEW --- Challenge 5: Network Task (Simulation)
void network_task(void *pvParameters) {
    stats_t current_stats;
    safe_printf("Network task started (waiting for stats...)\n");

    while(1) {
        // Wait for a new stats packet to arrive
        if(xQueueReceive(xStatsQueue, &current_stats, portMAX_DELAY) == pdPASS) {
            
            // Simulate network activity (e.g., WiFi connection, JSON formatting)
            vTaskDelay(pdMS_TO_TICKS(50)); // Small delay

            safe_printf("\n--- 📡 [NETWORK TASK] 📡 ---\n");
            safe_printf("Simulating MQTT Publish:\n");
            safe_printf("  { 'produced': %lu, 'consumed': %lu, 'dropped': %lu, 'qc_failed': %lu }\n", 
                       current_stats.produced, current_stats.consumed, 
                       current_stats.dropped, current_stats.failed_qc);
            safe_printf("-------------------------------\n\n");
        }
    }
}

// --- MODIFIED --- Statistics task
void statistics_task(void *pvParameters) {
    safe_printf("Statistics task started\n");
    
    while (1) {
        // Get backlog from all queues
        UBaseType_t itemsA = uxQueueMessagesWaiting(xQueueProductionA);
        UBaseType_t itemsB = uxQueueMessagesWaiting(xQueueProductionB);
        UBaseType_t itemsFinished = uxQueueMessagesWaiting(xQueueFinishedGoods);
        
        safe_printf("\n══════════ SYSTEM STATISTICS ══════════\n");
        safe_printf("Products Produced:  %lu\n", global_stats.produced);
        safe_printf("Products QC Failed: %lu\n", global_stats.failed_qc);
        safe_printf("Products Consumed:  %lu\n", global_stats.consumed);
        safe_printf("Products Dropped:   %lu\n", global_stats.dropped);
        
        // Efficiency: (Consumed) / (Total Produced)
        safe_printf("System Efficiency:  %.1f%%\n", 
                   global_stats.produced > 0 ? 
                   (float)global_stats.consumed / global_stats.produced * 100.0 : 0.0);
        safe_printf("--- QUEUE BACKLOGS ---\n");
        safe_printf("Queue Prod. A:    %d / %d\n", itemsA, QUEUE_A_LENGTH);
        safe_printf("Queue Prod. B:    %d / %d\n", itemsB, QUEUE_B_LENGTH);
        safe_printf("Queue Finished:   %d / %d\n", itemsFinished, FINISHED_GOODS_QUEUE_LENGTH);
        safe_printf("═════════════════════════════════════════\n\n");

        // --- NEW --- Challenge 5: Send latest stats to network task
        // Use Overwrite: always send the latest, don't block if network task is busy
        xQueueOverwrite(xStatsQueue, &global_stats);

        vTaskDelay(pdMS_TO_TICKS(5000)); // Report every 5 seconds
    }
}

// --- MODIFIED --- Load balancer task (now monitors inspectors)
void load_balancer_task(void *pvParameters) {
    const int MAX_BACKLOG = 8; // Threshold
    
    safe_printf("Load balancer started (monitoring Production queues)\n");
    
    while (1) {
        // --- MODIFIED --- Check total backlog *before* inspection
        UBaseType_t total_backlog = uxQueueMessagesWaiting(xQueueProductionA) + 
                                    uxQueueMessagesWaiting(xQueueProductionB);
        
        if (total_backlog > MAX_BACKLOG) {
            safe_printf("⚠️  HIGH LOAD DETECTED! Total Production Backlog: %d\n", total_backlog);
            safe_printf("💡 Suggestion: Add more *Inspectors* or speed up QC!\n");
            
            // Flash inspector LED as warning
            gpio_set_level(LED_INSPECTOR, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_INSPECTOR, 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000)); // Check every 2 seconds
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Smart Factory System Starting...");
    
    // Configure LED pins
    gpio_set_direction(LED_PRODUCER_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_INSPECTOR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_1, GPIO_MODE_OUTPUT);
    
    // Turn off all LEDs
    gpio_set_level(LED_PRODUCER_1, 0);
    gpio_set_level(LED_PRODUCER_2, 0);
    gpio_set_level(LED_PRODUCER_3, 0);
    gpio_set_level(LED_INSPECTOR, 0);
    gpio_set_level(LED_CONSUMER_1, 0);
    
    // Create mutex
    xPrintMutex = xSemaphoreCreateMutex();
    
    // --- MODIFIED --- Create all the new queues
    xQueueProductionA = xQueueCreate(QUEUE_A_LENGTH, sizeof(product_t));
    xQueueProductionB = xQueueCreate(QUEUE_B_LENGTH, sizeof(product_t));
    xQueueFinishedGoods = xQueueCreate(FINISHED_GOODS_QUEUE_LENGTH, sizeof(product_t));
    xStatsQueue = xQueueCreate(STATS_QUEUE_LENGTH, sizeof(stats_t));

    // --- NEW --- Challenge 3: Create and populate the Queue Set
    xProductionQueueSet = xQueueCreateSet(QUEUE_A_LENGTH + QUEUE_B_LENGTH);
    xQueueAddToSet(xQueueProductionA, xProductionQueueSet);
    xQueueAddToSet(xQueueProductionB, xProductionQueueSet);

    if (xQueueProductionA && xQueueProductionB && xQueueFinishedGoods && 
        xStatsQueue && xPrintMutex && xProductionQueueSet) {
        
        ESP_LOGI(TAG, "All Queues, Set, and Mutex created successfully");
        
        static int p1=1, p2=2, p3=3, c1=1;
        
        // Create 3 producer tasks
        xTaskCreate(producer_task, "Producer1", 3072, &p1, 3, NULL);
        xTaskCreate(producer_task, "Producer2", 3072, &p2, 3, NULL);
        xTaskCreate(producer_task, "Producer3", 3072, &p3, 3, NULL);
        
        // --- NEW --- Create 1 Inspector task
        xTaskCreate(inspector_task, "Inspector", 3072, NULL, 2, NULL);

        // --- MODIFIED --- Create 1 Consumer task (for batching)
        xTaskCreate(consumer_task, "Consumer1", 3072, &c1, 2, NULL);
        
        // Create monitoring tasks
        xTaskCreate(statistics_task, "Statistics", 3072, NULL, 1, NULL);
        xTaskCreate(load_balancer_task, "LoadBalancer", 2048, NULL, 1, NULL);
        
        // --- NEW --- Create network task
        xTaskCreate(network_task, "NetworkSim", 3072, NULL, 1, NULL);
        
        ESP_LOGI(TAG, "All tasks created. System operational.");
    } else {
        ESP_LOGE(TAG, "Failed to create one or more system components!");
    }
}
