#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h> // <<< CHALLENGE 5: For Atomic Operations
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h" // <<< CHALLENGE 3: For Performance Test

static const char *TAG = "ADV_MUTEX";

// --- Config ---
#define RUN_DEADLOCK_DEMO 1 // 1 = Run the BROKEN deadlock demo, 0 = Run the FIXED version

// --- LED Pins ---
#define LED_TASK1 GPIO_NUM_2
#define LED_TASK2 GPIO_NUM_4
#define LED_DEADLOCK GPIO_NUM_5
#define LED_CRITICAL GPIO_NUM_18

// --- CHALLENGE 1 & 4: Multiple/Recursive Mutex Handles ---
SemaphoreHandle_t xMutexA; // Now Recursive
SemaphoreHandle_t xMutexB; // For Deadlock demo

// --- CHALLENGE 4: Multiple Shared Resources ---
typedef struct {
    uint32_t counter;
    char shared_buffer[100];
    uint32_t checksum;
    uint32_t access_count;
} shared_resource_t;

shared_resource_t shared_data_A = {0, "", 0, 0};
shared_resource_t shared_data_B = {0, "", 0, 0}; // Resource B

// --- CHALLENGE 5: Lock-Free (Atomic) Resource ---
_Atomic uint32_t g_atomic_counter = 0;

// --- Statistics ---
typedef struct {
    uint32_t successful_access;
    uint32_t failed_access;
    uint32_t corruption_detected;
} access_stats_t;
access_stats_t stats = {0, 0, 0};

// --- Checksum Function ---
uint32_t calculate_checksum(const char* data, uint32_t counter) {
    uint32_t sum = counter;
    for (int i = 0; data[i] != '\0'; i++) {
        sum += (uint32_t)data[i] * (i + 1);
    }
    return sum;
}

// --- CHALLENGE 1: Recursive Mutex Demo Function ---
// This function is called *after* xMutexA is already taken
void recursive_helper_function(const char* task_name) {
    ESP_LOGI(TAG, "[%s] >> Inside recursive helper, taking mutex again...", task_name);
    
    // This will SUCCEED because xMutexA is a Recursive Mutex
    if (xSemaphoreTakeRecursive(xMutexA, pdMS_TO_TICKS(100)) == pdTRUE) {
        
        // Do some trivial work
        shared_data_A.access_count++; 
        
        ESP_LOGI(TAG, "[%s] >> Helper function complete, giving mutex...", task_name);
        xSemaphoreGiveRecursive(xMutexA);
    } else {
        ESP_LOGE(TAG, "[%s] >> FAILED to take recursive mutex!", task_name);
    }
}

// --- Main Critical Section Function (Now Fixed) ---
void access_resource_A(const char* task_name, gpio_num_t led_pin) {
    char temp_buffer[100];
    uint32_t temp_counter;
    uint32_t expected_checksum;
    
    ESP_LOGI(TAG, "[%s] Requesting access to Resource A...", task_name);
    
    // +++ FIX: Re-enabled Mutex Take +++
    // Using xSemaphoreTakeRecursive for Challenge 1
    if (xSemaphoreTakeRecursive(xMutexA, pdMS_TO_TICKS(5000)) == pdTRUE) {
        ESP_LOGI(TAG, "[%s] ✓ Mutex A acquired - entering critical section", task_name);
        stats.successful_access++;
        
        gpio_set_level(led_pin, 1);
        gpio_set_level(LED_CRITICAL, 1);
        
        // === CRITICAL SECTION BEGINS ===
        
        // Read current state
        temp_counter = shared_data_A.counter;
        strcpy(temp_buffer, shared_data_A.shared_buffer);
        expected_checksum = shared_data_A.checksum;
        
        // Verify data integrity (should NOT fail now)
        uint32_t calculated_checksum = calculate_checksum(temp_buffer, temp_counter);
        if (calculated_checksum != expected_checksum && shared_data_A.access_count > 0) {
            ESP_LOGE(TAG, "[%s] ⚠️  DATA CORRUPTION DETECTED!", task_name);
            stats.corruption_detected++;
        }
        
        vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 300))); // Simulate work
        
        // Modify shared data
        shared_data_A.counter = temp_counter + 1;
        snprintf(shared_data_A.shared_buffer, sizeof(shared_data_A.shared_buffer), 
                 "Modified by %s #%lu", task_name, shared_data_A.counter);
        shared_data_A.checksum = calculate_checksum(shared_data_A.shared_buffer, shared_data_A.counter);
        
        // --- CHALLENGE 1: Call the recursive helper ---
        recursive_helper_function(task_name); 
        // Note: access_count is now incremented inside the helper

        vTaskDelay(pdMS_TO_TICKS(100 + (esp_random() % 200))); // More work
        
        // === CRITICAL SECTION ENDS ===
        
        gpio_set_level(led_pin, 0);
        gpio_set_level(LED_CRITICAL, 0);
        
        // +++ FIX: Re-enabled Mutex Give +++
        xSemaphoreGiveRecursive(xMutexA);
        ESP_LOGI(TAG, "[%s] Mutex A released", task_name);
        
    } else {
        ESP_LOGW(TAG, "[%s] ✗ Failed to acquire mutex A (timeout)", task_name);
        stats.failed_access++;
    }
}

// --- Standard Tasks using Resource A ---
void high_priority_task(void *pvParameters) {
    ESP_LOGI(TAG, "High Priority Task started (Prio 5)");
    while (1) {
        access_resource_A("HIGH_PRI", LED_TASK1);
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 3000)));
    }
}

void low_priority_task(void *pvParameters) {
    ESP_LOGI(TAG, "Low Priority Task started (Prio 2)");
    while (1) {
        access_resource_A("LOW_PRI", LED_TASK2);
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 1000)));
    }
}

// --- CHALLENGE 5: Atomic (Lock-Free) Task ---
void atomic_task(void *pvParameters) {
    ESP_LOGI(TAG, "Atomic Task started (Prio 3)");
    while(1) {
        // This is a thread-safe increment WITHOUT a mutex!
        atomic_fetch_add(&g_atomic_counter, 1);
        vTaskDelay(pdMS_TO_TICKS(100 + (esp_random() % 100))); // Runs very fast
    }
}

// --- CHALLENGE 2: Deadlock Demonstration Tasks ---
void deadlock_task_A(void *pvParameters) {
    ESP_LOGW(TAG, "Deadlock Task A started (Prio 3)");
    while(1) {
        ESP_LOGI(TAG, "[Task A] Taking Mutex A...");
        xSemaphoreTakeRecursive(xMutexA, portMAX_DELAY);
        ESP_LOGI(TAG, "[Task A] ✓ Got Mutex A");
        
        gpio_set_level(LED_DEADLOCK, 1);
        vTaskDelay(pdMS_TO_TICKS(100)); // Context switch to Task B

        ESP_LOGW(TAG, "[Task A] !! Trying to take Mutex B... (will block)");
        xSemaphoreTakeRecursive(xMutexB, portMAX_DELAY); // Will wait for Task B
        
        // --- This code will never be reached ---
        ESP_LOGI(TAG, "[Task A] ✓ Got Mutex B");
        
        // Release
        xSemaphoreGiveRecursive(xMutexB);
        gpio_set_level(LED_DEADLOCK, 0);
        xSemaphoreGiveRecursive(xMutexA);
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void deadlock_task_B_BROKEN(void *pvParameters) {
    ESP_LOGW(TAG, "Deadlock Task B (BROKEN) started (Prio 3)");
    while(1) {
        ESP_LOGI(TAG, "[Task B] Taking Mutex B...");
        xSemaphoreTakeRecursive(xMutexB, portMAX_DELAY);
        ESP_LOGI(TAG, "[Task B] ✓ Got Mutex B");
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Context switch back to Task A

        ESP_LOGW(TAG, "[Task B] !! Trying to take Mutex A... (DEADLOCK!)");
        xSemaphoreTakeRecursive(xMutexA, portMAX_DELAY); // Will wait for Task A
        
        // --- This code will never be reached ---
        ESP_LOGI(TAG, "[Task B] ✓ Got Mutex A");
        
        // Release
        xSemaphoreGiveRecursive(xMutexA);
        xSemaphoreGiveRecursive(xMutexB);
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void deadlock_task_B_FIXED(void *pvParameters) {
    ESP_LOGW(TAG, "Deadlock Task B (FIXED) started (Prio 3)");
    while(1) {
        ESP_LOGI(TAG, "[Task B-FIXED] Taking Mutex A (Correct Order)...");
        xSemaphoreTakeRecursive(xMutexA, portMAX_DELAY);
        ESP_LOGI(TAG, "[Task B-FIXED] ✓ Got Mutex A");
        
        vTaskDelay(pdMS_TO_TICKS(100)); 

        ESP_LOGI(TAG, "[Task B-FIXED] Taking Mutex B (Correct Order)...");
        xSemaphoreTakeRecursive(xMutexB, portMAX_DELAY); // Will not deadlock
        ESP_LOGI(TAG, "[Task B-FIXED] ✓ Got Mutex B");
        
        ESP_LOGI(TAG, "[Task B-FIXED] Doing work with both mutexes...");
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Release in reverse order
        ESP_LOGI(TAG, "[Task B-FIXED] Releasing Mutex B");
        xSemaphoreGiveRecursive(xMutexB);
        ESP_LOGI(TAG, "[Task B-FIXED] Releasing Mutex A");
        xSemaphoreGiveRecursive(xMutexA);
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// --- CHALLENGE 3: Performance Test Task ---
void performance_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "--- Performance Test Started ---");
    
    const int iterations = 10000;
    uint64_t start_time, end_time;
    uint32_t total_time;
    volatile uint32_t unprotected_counter = 0;

    // 1. Mutex Overhead
    start_time = esp_timer_get_time();
    for(int i=0; i<iterations; i++) {
        xSemaphoreTakeRecursive(xMutexA, portMAX_DELAY);
        xSemaphoreGiveRecursive(xMutexA);
    }
    end_time = esp_timer_get_time();
    total_time = end_time - start_time;
    ESP_LOGW(TAG, "PERF: Mutex Take/Give avg: %.2f us", (float)total_time / iterations);

    // 2. Atomic Operation Overhead
    start_time = esp_timer_get_time();
    for(int i=0; i<iterations; i++) {
        atomic_fetch_add(&g_atomic_counter, 1);
    }
    end_time = esp_timer_get_time();
    total_time = end_time - start_time;
    ESP_LOGW(TAG, "PERF: Atomic Increment avg: %.2f us", (float)total_time / iterations);

    // 3. Unprotected Overhead (Baseline)
    start_time = esp_timer_get_time();
    for(int i=0; i<iterations; i++) {
        unprotected_counter++;
    }
    end_time = esp_timer_get_time();
    total_time = end_time - start_time;
    ESP_LOGW(TAG, "PERF: Unprotected Inc avg: %.2f us", (float)total_time / iterations);

    ESP_LOGI(TAG, "--- Performance Test Finished ---");
    vTaskDelete(NULL); // Delete self
}

// --- System Monitor Task (Updated) ---
void monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "System monitor started (Prio 1)");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Every 10 seconds
        
        ESP_LOGI(TAG, "\n═══ ADVANCED MUTEX MONITOR ═══");
        ESP_LOGI(TAG, "Mutex A Available: %s (Owner: %p)", 
                 uxSemaphoreGetCount(xMutexA) ? "YES" : "NO", xSemaphoreGetMutexHolder(xMutexA));
        ESP_LOGI(TAG, "Mutex B Available: %s (Owner: %p)", 
                 uxSemaphoreGetCount(xMutexB) ? "YES" : "NO", xSemaphoreGetMutexHolder(xMutexB));

        // Check for deadlock
        if (xSemaphoreGetMutexHolder(xMutexA) != NULL && xSemaphoreGetMutexHolder(xMutexB) != NULL) {
             ESP_LOGE(TAG, "!!!! DEADLOCK DETECTED !!!!");
             gpio_set_level(LED_DEADLOCK, 1);
        }
        
        ESP_LOGI(TAG, "Shared Resource A State:");
        ESP_LOGI(TAG, "  Counter: %lu", shared_data_A.counter);
        ESP_LOGI(TAG, "  Access Count: %lu", shared_data_A.access_count);
        
        ESP_LOGI(TAG, "Lock-Free Atomic Counter: %lu", atomic_load(&g_atomic_counter));
        
        // Verify data integrity
        uint32_t current_checksum = calculate_checksum(shared_data_A.shared_buffer, shared_data_A.counter);
        if (current_checksum != shared_data_A.checksum && shared_data_A.access_count > 0) {
            ESP_LOGE(TAG, "⚠️  MONITOR: CORRUPTION DETECTED IN RESOURCE A!");
            stats.corruption_detected++;
        } else if (shared_data_A.access_count > 0) {
             ESP_LOGI(TAG, "✓ Monitor: Resource A checksum is valid.");
        }
        
        ESP_LOGI(TAG, "Access Statistics:");
        ESP_LOGI(TAG, "  Successful: %lu", stats.successful_access);
        ESP_LOGI(TAG, "  Failed:     %lu", stats.failed_access);
        ESP_LOGI(TAG, "  Corrupted:  %lu", stats.corruption_detected);
        ESP_LOGI(TAG, "══════════════════════════════\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Advanced Mutex Lab Starting...");
    
    // Config LEDs
    gpio_set_direction(LED_TASK1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_DEADLOCK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CRITICAL, GPIO_MODE_OUTPUT);
    
    // Create Mutexes
    // CHALLENGE 1
    xMutexA = xSemaphoreCreateRecursiveMutex();
    // CHALLENGE 2 & 4
    xMutexB = xSemaphoreCreateRecursiveMutex(); 
    
    if (xMutexA != NULL && xMutexB != NULL) {
        ESP_LOGI(TAG, "Recursive Mutexes (A & B) created successfully");
        
        // Initialize shared resource A
        shared_data_A.counter = 0;
        strcpy(shared_data_A.shared_buffer, "Initial state");
        shared_data_A.checksum = calculate_checksum(shared_data_A.shared_buffer, shared_data_A.counter);
        shared_data_A.access_count = 0;
        
        // Create tasks
        // --- Standard tasks ---
        xTaskCreate(high_priority_task, "HighPri", 3072, NULL, 5, NULL); // Prio 5
        xTaskCreate(low_priority_task, "LowPri", 3072, NULL, 2, NULL);  // Prio 2
        
        // --- CHALLENGE 5: Atomic Task ---
        xTaskCreate(atomic_task, "AtomicTask", 2048, NULL, 3, NULL);

        // --- CHALLENGE 2: Deadlock Tasks ---
        xTaskCreate(deadlock_task_A, "Deadlock_A", 3072, NULL, 3, NULL);
        #if RUN_DEADLOCK_DEMO
            ESP_LOGW(TAG, "!!! WARNING: Running BROKEN deadlock demo. System will hang. !!!");
            xTaskCreate(deadlock_task_B_BROKEN, "Deadlock_B", 3072, NULL, 3, NULL);
        #else
            ESP_LOGI(TAG, "Running FIXED deadlock demo (using lock ordering).");
            xTaskCreate(deadlock_task_B_FIXED, "Deadlock_B_Fixed", 3072, NULL, 3, NULL);
        #endif

        // --- Monitoring Tasks ---
        xTaskCreate(monitor_task, "Monitor", 3072, NULL, 1, NULL); // Prio 1 (Lowest)
        
        // --- CHALLENGE 3: Performance Test ---
        xTaskCreate(performance_test_task, "PerfTest", 2048, NULL, 1, NULL);
        
        ESP_LOGI(TAG, "All tasks created. System operational.");
        
    } else {
        ESP_LOGE(TAG, "Failed to create mutexes!");
    }
}
