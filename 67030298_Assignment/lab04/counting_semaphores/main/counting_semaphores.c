#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "ADV_POOL";

// LED pins
#define LED_RESOURCE_1 GPIO_NUM_2
#define LED_RESOURCE_2 GPIO_NUM_4
#define LED_RESOURCE_3 GPIO_NUM_5
#define LED_PRODUCER GPIO_NUM_18
#define LED_SYSTEM GPIO_NUM_19

// Configuration
#define MAX_RESOURCES 5      // Maximum *physical* resources
#define INITIAL_POOL_SIZE 3  // CHALLENGE 3: Starting pool size
#define NUM_PRODUCERS 4      // 2 Regular, 2 VIP

// Semaphore handles
SemaphoreHandle_t xCountingSemaphore; // The "gatekeeper" for resource count
SemaphoreHandle_t xResourceMutex;     // CRITICAL: Mutex to protect the resources array

// CHALLENGE 2: Resource Health
typedef enum {
    HEALTHY,
    BROKEN
} resource_health_t;

// Resource management struct
typedef struct {
    int resource_id;
    bool in_use;
    char current_user[20];
    uint32_t usage_count;        // For Challenge 5 (Fairness)
    uint32_t total_usage_time;
    resource_health_t health;    // For Challenge 2 (Health)
} resource_t;

// Initialize all 5 resources
resource_t resources[MAX_RESOURCES] = {
    {1, false, "", 0, 0, HEALTHY},
    {2, false, "", 0, 0, HEALTHY},
    {3, false, "", 0, 0, HEALTHY},
    {4, false, "", 0, 0, HEALTHY},
    {5, false, "", 0, 0, HEALTHY}
};

// System statistics
typedef struct {
    uint32_t total_requests;
    uint32_t successful_acquisitions;
    uint32_t failed_acquisitions;
} system_stats_t;

system_stats_t stats = {0, 0, 0};

// CHALLENGE 3: Dynamic Sizing
volatile uint32_t g_logical_pool_size = INITIAL_POOL_SIZE;

// --- CRITICAL SECTION FUNCTIONS (THREAD-SAFE) ---

/**
 * @brief Acquires the "best" available resource.
 * THREAD-SAFE: Must be called *after* xSemaphoreTake(xCountingSemaphore).
 * CHALLENGE 2: Checks for HEALTHY.
 * CHALLENGE 5: Implements fair scheduling (picks resource with lowest usage).
 * @param user_name Name of the task acquiring the resource.
 * @return Resource index (0 to MAX_RESOURCES-1) or -1 if none found.
 */
int acquire_resource(const char* user_name) {
    int best_res_idx = -1;
    uint32_t min_usage = UINT32_MAX;

    // CRITICAL: Lock the resource array
    if (xSemaphoreTake(xResourceMutex, portMAX_DELAY) == pdTRUE) {
        
        // CHALLENGE 5: Find the least-used, healthy, free resource
        for (int i = 0; i < MAX_RESOURCES; i++) {
            if (!resources[i].in_use && resources[i].health == HEALTHY) {
                if (resources[i].usage_count < min_usage) {
                    min_usage = resources[i].usage_count;
                    best_res_idx = i;
                }
            }
        }

        if (best_res_idx != -1) {
            // Found one, mark it as in use
            resources[best_res_idx].in_use = true;
            strcpy(resources[best_res_idx].current_user, user_name);
            resources[best_res_idx].usage_count++;
            
            // Turn on LED
            if (best_res_idx < 3) { // Only have 3 LEDs defined
                 switch (best_res_idx) {
                    case 0: gpio_set_level(LED_RESOURCE_1, 1); break;
                    case 1: gpio_set_level(LED_RESOURCE_2, 1); break;
                    case 2: gpio_set_level(LED_RESOURCE_3, 1); break;
                }
            }
        }
        
        // CRITICAL: Unlock the resource array
        xSemaphoreGive(xResourceMutex);
    }
    
    return best_res_idx;
}

/**
 * @brief Releases a resource.
 * THREAD-SAFE.
 * @param resource_index Index of resource to release.
 * @param usage_time Time it was used (for stats).
 */
void release_resource(int resource_index, uint32_t usage_time) {
    if (resource_index < 0 || resource_index >= MAX_RESOURCES) {
        return;
    }

    // CRITICAL: Lock the resource array
    if (xSemaphoreTake(xResourceMutex, portMAX_DELAY) == pdTRUE) {
        resources[resource_index].in_use = false;
        strcpy(resources[resource_index].current_user, "");
        resources[resource_index].total_usage_time += usage_time;
        
        // Turn off LED
        if (resource_index < 3) {
            switch (resource_index) {
                case 0: gpio_set_level(LED_RESOURCE_1, 0); break;
                case 1: gpio_set_level(LED_RESOURCE_2, 0); break;
                case 2: gpio_set_level(LED_RESOURCE_3, 0); break;
            }
        }

        // CRITICAL: Unlock the resource array
        xSemaphoreGive(xResourceMutex);
    }
}

// --- TASK DEFINITIONS ---

// CHALLENGE 1: Producer task (Regular and VIP)
void producer_task(void *pvParameters) {
    int producer_id = (int)pvParameters;
    int priority = uxTaskPriorityGet(NULL); // Get own priority
    char task_name[20];
    snprintf(task_name, sizeof(task_name), "Producer%d(P%d)", producer_id, priority);
    
    ESP_LOGI(TAG, "%s started", task_name);
    
    while (1) {
        // --- 1. Request access to the pool ---
        xSemaphoreTake(xResourceMutex, portMAX_DELAY);
        stats.total_requests++;
        xSemaphoreGive(xResourceMutex);
        
        ESP_LOGI(TAG, "🏭 %s: Requesting resource...", task_name);
        gpio_set_level(LED_PRODUCER, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_PRODUCER, 0);
        
        uint32_t start_time = xTaskGetTickCount();
        
        // Try to acquire counting semaphore (resource from pool)
        if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(8000)) == pdTRUE) {
            uint32_t wait_time = (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS;
            
            // --- 2. We got a token! Now find the actual resource ---
            int resource_idx = acquire_resource(task_name);
            
            if (resource_idx >= 0) {
                // --- 3. Got the resource, use it ---
                xSemaphoreTake(xResourceMutex, portMAX_DELAY);
                stats.successful_acquisitions++;
                xSemaphoreGive(xResourceMutex);

                ESP_LOGI(TAG, "✓ %s: Acquired resource %d (wait: %lums)", 
                         task_name, resource_idx + 1, wait_time);
                
                uint32_t usage_time = 1000 + (esp_random() % 3000); // 1-4 seconds
                ESP_LOGI(TAG, "🔧 %s: Using resource %d for %lums", 
                         task_name, resource_idx + 1, usage_time);
                
                vTaskDelay(pdMS_TO_TICKS(usage_time));
                
                // --- 4. Release the resource ---
                release_resource(resource_idx, usage_time);
                ESP_LOGI(TAG, "✓ %s: Released resource %d", task_name, resource_idx + 1);
                
                // --- 5. Give back the token to the pool ---
                xSemaphoreGive(xCountingSemaphore);
                
            } else {
                // This happens if a resource breaks (Challenge 2)
                ESP_LOGE(TAG, "✗ %s: Semaphore acquired but no HEALTHY resource available!", task_name);
                xSemaphoreGive(xCountingSemaphore); // Give back token immediately
            }
            
        } else {
            // --- Failed to get a token (pool busy or small) ---
            xSemaphoreTake(xResourceMutex, portMAX_DELAY);
            stats.failed_acquisitions++;
            xSemaphoreGive(xResourceMutex);
            ESP_LOGW(TAG, "⏰ %s: Timeout waiting for resource", task_name);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000))); // 2-5 seconds
    }
}

// CHALLENGE 4: Greedy Producer Task (Reservation/Hoarding)
void greedy_producer_task(void *pvParameters) {
    char task_name[] = "GreedyProd(P4)";
    ESP_LOGW(TAG, "💰 %s started. Will try to hoard 3 resources.", task_name);

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000 + (esp_random() % 10000))); // 10-20 sec
        ESP_LOGW(TAG, "💰 %s: Attempting to hoard 3 resources...", task_name);

        int res_idx[3] = {-1, -1, -1};
        bool success = true;

        // Try to take 3 tokens
        for (int i = 0; i < 3; i++) {
            if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(1000)) == pdFALSE) {
                ESP_LOGE(TAG, "💰 %s: Failed to get token %d. Releasing all.", task_name, i+1);
                success = false;
                // Give back any tokens we successfully took
                for(int j = 0; j < i; j++) {
                    xSemaphoreGive(xCountingSemaphore);
                }
                break;
            }
        }

        if (success) {
            // We have 3 tokens, now acquire the physical resources
            for(int i=0; i<3; i++) {
                res_idx[i] = acquire_resource(task_name);
                if(res_idx[i] == -1) {
                     ESP_LOGE(TAG, "💰 %s: Got token but no resource! Releasing.", task_name);
                     // (Simplified: just give back tokens)
                }
            }
            
            ESP_LOGW(TAG, "💰 %s: Successfully hoarding resources [%d, %d, %d] for 5s", 
                     task_name, res_idx[0]+1, res_idx[1]+1, res_idx[2]+1);
            vTaskDelay(pdMS_TO_TICKS(5000)); // Hold them

            // Release all
            for(int i=0; i<3; i++) {
                if(res_idx[i] != -1) {
                    release_resource(res_idx[i], 5000);
                    xSemaphoreGive(xCountingSemaphore);
                }
            }
            ESP_LOGW(TAG, "💰 %s: Released all resources.", task_name);
        }
    }
}


// CHALLENGE 2: Chaos Monkey Task
void chaos_monkey_task(void *pvParameters) {
    ESP_LOGE(TAG, "🐒 Chaos Monkey task started. Will break resources randomly.");
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(15000 + (esp_random() % 15000))); // 15-30 sec
        
        int res_to_break = -1;

        // CRITICAL: Lock resources to find one to break
        if (xSemaphoreTake(xResourceMutex, portMAX_DELAY) == pdTRUE) {
            
            // Find a random, healthy, non-used resource
            int start_idx = esp_random() % MAX_RESOURCES;
            for (int i = 0; i < MAX_RESOURCES; i++) {
                int idx = (start_idx + i) % MAX_RESOURCES;
                if (!resources[idx].in_use && resources[idx].health == HEALTHY) {
                    res_to_break = idx;
                    break;
                }
            }

            if (res_to_break != -1) {
                // Found one! Mark it as broken
                resources[res_to_break].health = BROKEN;
                ESP_LOGE(TAG, "🐒 CHAOS: Resource %d marked as BROKEN!", res_to_break + 1);
            }
            xSemaphoreGive(xResourceMutex);
        }

        if (res_to_break != -1) {
            // Now, permanently take one token from the pool to reflect this
            if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
                ESP_LOGE(TAG, "🐒 CHAOS: Successfully removed 1 token from pool for broken resource.");
            } else {
                ESP_LOGE(TAG, "🐒 CHAOS: Could not remove token (pool already empty?)");
            }
        }
    }
}

// CHALLENGE 3: System Manager Task (replaces load_generator)
void system_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "System Manager task started. (Prio 5)");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check load every 10 seconds

        uint32_t failed_count = 0;

        // Get and reset failure count
        xSemaphoreTake(xResourceMutex, portMAX_DELAY);
        failed_count = stats.failed_acquisitions;
        stats.failed_acquisitions = 0; // Reset counter
        xSemaphoreGive(xResourceMutex);

        // --- Dynamic Sizing Logic ---
        if (failed_count > 3 && g_logical_pool_size < MAX_RESOURCES) {
            // High load! Try to increase pool size
            if (xSemaphoreGive(xCountingSemaphore) == pdTRUE) {
                g_logical_pool_size++;
                ESP_LOGW(TAG, "🚀 MANAGER: High load! Pool size increased to %lu", g_logical_pool_size);
                gpio_set_level(LED_SYSTEM, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_SYSTEM, 0);
            }
        } else if (failed_count == 0 && g_logical_pool_size > 2) {
            // Low load! Try to shrink pool size (min 2)
            if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_logical_pool_size--;
                ESP_LOGI(TAG, "💤 MANAGER: Low load. Pool size decreased to %lu", g_logical_pool_size);
            }
        }
    }
}

// Resource monitor task (Thread-safe)
void resource_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Resource monitor started (Prio 2)");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(6000)); // Every 6 seconds
        
        int available_count = uxSemaphoreGetCount(xCountingSemaphore);
        
        ESP_LOGI(TAG, "\n📊 RESOURCE POOL STATUS (Logical size: %lu)", g_logical_pool_size);
        ESP_LOGI(TAG, "Available tokens: %d/%lu", available_count, g_logical_pool_size);
        
        char pool_visual[MAX_RESOURCES + 1] = {0};

        // CRITICAL: Lock resources to read their status
        if (xSemaphoreTake(xResourceMutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < MAX_RESOURCES; i++) {
                if (resources[i].health == BROKEN) {
                    pool_visual[i] = 'X'; // Broken
                    ESP_LOGE(TAG, "  Resource %d: [X] BROKEN (Usage: %lu)", 
                             i + 1, resources[i].usage_count);
                } else if (resources[i].in_use) {
                    pool_visual[i] = '■'; // In use
                    ESP_LOGI(TAG, "  Resource %d: [■] BUSY (User: %s, Usage: %lu)", 
                             i + 1, resources[i].current_user, resources[i].usage_count);
                } else {
                    pool_visual[i] = '□'; // Free
                    ESP_LOGI(TAG, "  Resource %d: [□] FREE (Usage: %lu)", 
                             i + 1, resources[i].usage_count);
                }
            }
            xSemaphoreGive(xResourceMutex);
        }
        
        printf("Pool: [%s] Available: %d\n", pool_visual, available_count);
        ESP_LOGI(TAG, "═══════════════════════════\n");
    }
}

// System statistics task (Thread-safe)
void statistics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Statistics task started (Prio 1)");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(12000)); // Every 12 seconds
        
        // Copy stats in a thread-safe way
        system_stats_t current_stats;
        uint32_t total_usage_count = 0;

        if (xSemaphoreTake(xResourceMutex, portMAX_DELAY) == pdTRUE) {
            current_stats = stats;
            // Reset totals for next period
            stats.total_requests = 0;
            stats.successful_acquisitions = 0;
            // failed_acquisitions is reset by manager task

            ESP_LOGI(TAG, "\n📈 SYSTEM STATISTICS (Last 12s)");
            ESP_LOGI(TAG, "Total requests: %lu", current_stats.total_requests);
            ESP_LOGI(TAG, "Successful acquisitions: %lu", current_stats.successful_acquisitions);
            
            if (current_stats.total_requests > 0) {
                float success_rate = (float)current_stats.successful_acquisitions / current_stats.total_requests * 100;
                ESP_LOGI(TAG, "Success rate: %.1f%%", success_rate);
            }

            // Resource utilization statistics
            ESP_LOGI(TAG, "Resource utilization (Fairness check):");
            for (int i = 0; i < MAX_RESOURCES; i++) {
                total_usage_count += resources[i].usage_count;
                ESP_LOGI(TAG, "  Resource %d: %lu total uses (Time: %lums)", 
                       i + 1, resources[i].usage_count, resources[i].total_usage_time);
            }
            ESP_LOGI(TAG, "Total resource usage events: %lu", total_usage_count);
            ESP_LOGI(TAG, "════════════════════════════\n");

            xSemaphoreGive(xResourceMutex);
        }
    }
}


void app_main(void) {
    ESP_LOGI(TAG, "Advanced Resource Pool Lab Starting...");
    
    // Config LEDs
    gpio_set_direction(LED_RESOURCE_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM, GPIO_MODE_OUTPUT);
    
    // Create counting semaphore (start with FULL count)
    xCountingSemaphore = xSemaphoreCreateCounting(MAX_RESOURCES, MAX_RESOURCES);
    
    // Create the resource mutex (CRITICAL)
    xResourceMutex = xSemaphoreCreateMutex();
    
    if (xCountingSemaphore != NULL && xResourceMutex != NULL) {
        ESP_LOGI(TAG, "Semaphore and Mutex created (Max count: %d)", MAX_RESOURCES);

        // CHALLENGE 3: "Hold back" resources to set initial size
        int tokens_to_hold = MAX_RESOURCES - INITIAL_POOL_SIZE;
        for (int i = 0; i < tokens_to_hold; i++) {
            xSemaphoreTake(xCountingSemaphore, 0);
        }
        ESP_LOGI(TAG, "Set initial pool size to %d by holding %d tokens.", 
                 INITIAL_POOL_SIZE, tokens_to_hold);

        
        static int producer_ids[NUM_PRODUCERS];
        
        // CHALLENGE 1: Create producers with different priorities
        for (int i = 0; i < NUM_PRODUCERS; i++) {
            producer_ids[i] = i + 1;
            char task_name[20];
            snprintf(task_name, sizeof(task_name), "Producer%d", i + 1);
            
            // First half are REGULAR (Prio 2), second half are VIP (Prio 4)
            int priority = (i < (NUM_PRODUCERS / 2)) ? 2 : 4; 
            xTaskCreate(producer_task, task_name, 3072, &producer_ids[i], priority, NULL);
        }
        
        // Create other tasks
        xTaskCreate(resource_monitor_task, "ResMonitor", 3072, NULL, 2, NULL);
        xTaskCreate(statistics_task, "Statistics", 3072, NULL, 1, NULL); // Lowest prio
        xTaskCreate(system_manager_task, "SysManager", 2048, NULL, 5, NULL); // Highest prio
        xTaskCreate(chaos_monkey_task, "ChaosMonkey", 2048, NULL, 1, NULL); // Low prio
        xTaskCreate(greedy_producer_task, "GreedyProd", 3072, NULL, 4, NULL); // CHALLENGE 4

        ESP_LOGI(TAG, "System created with:");
        ESP_LOGI(TAG, "Max Resources: %d", MAX_RESOURCES);
        ESP_LOGI(TAG, "Initial Pool: %d", INITIAL_POOL_SIZE);
        ESP_LOGI(TAG, "Producers: %d (%d Prio2, %d Prio4)", 
                 NUM_PRODUCERS, (NUM_PRODUCERS / 2), (NUM_PRODUCERS - (NUM_PRODUCERS / 2)));
        
    } else {
        ESP_LOGE(TAG, "Failed to create semaphore or mutex!");
    }
}
