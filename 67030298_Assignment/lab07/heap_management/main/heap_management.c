#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_random.h"
static const char *TAG = "HEAP_MGMT";
// GPIO สำหรับแสดงสถานะ
#define LED_MEMORY_OK       GPIO_NUM_2    // Memory system OK
#define LED_LOW_MEMORY      GPIO_NUM_4    // Low memory warning
#define LED_MEMORY_ERROR    GPIO_NUM_5    // Memory error/leak
#define LED_FRAGMENTATION   GPIO_NUM_18   // High fragmentation
#define LED_SPIRAM_ACTIVE   GPIO_NUM_19   // SPIRAM usage
// Memory thresholds
#define LOW_MEMORY_THRESHOLD      50000   // 50KB
#define CRITICAL_MEMORY_THRESHOLD 20000   // 20KB
#define FRAGMENTATION_THRESHOLD 0.3     // 30% fragmentation
#define MAX_ALLOCATIONS         200
// Memory allocation tracking
typedef struct {
    void* ptr;
    size_t size;
    uint32_t caps;
    const char* description;
    uint64_t timestamp;
    bool is_active;
} memory_allocation_t;
// Memory statistics
typedef struct {
    uint32_t total_allocations;
    uint32_t total_deallocations;
    uint32_t current_allocations;
    uint64_t total_bytes_allocated;
    uint64_t total_bytes_deallocated;
    uint64_t peak_usage;
    uint32_t allocation_failures;
    uint32_t fragmentation_events;
    uint32_t low_memory_events;
} memory_stats_t;
// Global variables
static memory_allocation_t allocations[MAX_ALLOCATIONS];
static memory_stats_t stats = {0};
static SemaphoreHandle_t memory_mutex;
static bool memory_monitoring_enabled = true;
// Memory monitoring functions
int find_free_allocation_slot(void) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (!allocations[i].is_active) {
            return i;
        }
    }
    return -1;
}
int find_allocation_by_ptr(void* ptr) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].is_active && allocations[i].ptr == ptr) {
            return i;
        }
    }
    return -1;
}
void* tracked_malloc(size_t size, uint32_t caps, const char* description) {
    void* ptr = heap_caps_malloc(size, caps);
    if (memory_monitoring_enabled && memory_mutex) {
        if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (ptr) {
                int slot = find_free_allocation_slot();
                if (slot >= 0) {
                    allocations[slot].ptr = ptr;
                    allocations[slot].size = size;
                    allocations[slot].caps = caps;
                    allocations[slot].description = description;
                    allocations[slot].timestamp = esp_timer_get_time();
                    allocations[slot].is_active = true;
                    stats.total_allocations++;
                    stats.current_allocations++;
                    stats.total_bytes_allocated += size;
                    size_t current_usage = stats.total_bytes_allocated - stats.total_bytes_deallocated;
                    if (current_usage > stats.peak_usage) {
                        stats.peak_usage = current_usage;
                    }
                    ESP_LOGI(TAG, "✅ Allocated %d bytes at %p (%s) - Slot %d", 
                             size, ptr, description, slot);
                } else {
                    ESP_LOGW(TAG, "⚠️ Allocation tracking full!");
                }
            } else {
                stats.allocation_failures++;
                ESP_LOGE(TAG, "❌ Failed to allocate %d bytes (%s)", size, description);
            }
            xSemaphoreGive(memory_mutex);
        }
    }
    return ptr;
}
void tracked_free(void* ptr, const char* description) {
    if (!ptr) return;
    if (memory_monitoring_enabled && memory_mutex) {
        if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            int slot = find_allocation_by_ptr(ptr);
            if (slot >= 0) {
                allocations[slot].is_active = false;
                stats.total_deallocations++;
                stats.current_allocations--;
                stats.total_bytes_deallocated += allocations[slot].size;
                ESP_LOGI(TAG, "🗑️ Freed %d bytes at %p (%s) - Slot %d", 
                         allocations[slot].size, ptr, description, slot);
            } else {
                ESP_LOGW(TAG, "⚠️ Freeing untracked pointer %p (%s)", ptr, description);
            }
            xSemaphoreGive(memory_mutex);
        }
    }
    heap_caps_free(ptr);
}
// --- Batch Memory ---
typedef struct {
    void* ptrs[10];
    size_t sizes[10];
    int count;
    const char* batch_name;
} memory_batch_t;
memory_batch_t* create_memory_batch(const char* name) {
    memory_batch_t* batch = tracked_malloc(sizeof(memory_batch_t), 
                                          MALLOC_CAP_INTERNAL, "BatchStruct");
    if (batch) {
        memset(batch, 0, sizeof(memory_batch_t));
        batch->batch_name = name;
    }
    return batch;
}
bool batch_allocate(memory_batch_t* batch, size_t size, uint32_t caps) {
    if (!batch || batch->count >= 10) return false;
    batch->ptrs[batch->count] = tracked_malloc(size, caps, batch->batch_name);
    if (batch->ptrs[batch->count]) {
        batch->sizes[batch->count] = size;
        batch->count++;
        return true;
    }
    return false;
}
void batch_free(memory_batch_t* batch) {
    if (!batch) return;
    for (int i = 0; i < batch->count; i++) {
        if (batch->ptrs[i]) {
            tracked_free(batch->ptrs[i], batch->batch_name);
            batch->ptrs[i] = NULL;
        }
    }
    tracked_free(batch, "BatchStruct");
}
// --- Custom Memory Allocator (Slab) ---
typedef struct {
    uint64_t timestamp;
    float temperature;
    float humidity;
} sensor_data_t;
#define SENSOR_DATA_POOL_SIZE 50
typedef struct {
    void* memory_block;
    size_t item_size;
    int total_items;
    bool* free_slots;
    SemaphoreHandle_t lock;
    const char* pool_name;
} slab_pool_t;
static const char* SLAB_TAG = "SLAB";
slab_pool_t* slab_pool_create(const char* name, size_t item_size, int num_items) {
    slab_pool_t* pool = tracked_malloc(sizeof(slab_pool_t), MALLOC_CAP_INTERNAL, "SlabPoolStruct");
    if (!pool) return NULL;
    pool->item_size = item_size;
    pool->total_items = num_items;
    pool->pool_name = name;
    size_t total_mem_size = item_size * num_items;
    pool->memory_block = tracked_malloc(total_mem_size, MALLOC_CAP_INTERNAL, name);
    pool->free_slots = tracked_malloc(sizeof(bool) * num_items, MALLOC_CAP_INTERNAL, "SlabBitmap");
    pool->lock = xSemaphoreCreateMutex();
    if (!pool->memory_block || !pool->free_slots || !pool->lock) {
        if (pool->memory_block) tracked_free(pool->memory_block, name);
        if (pool->free_slots) tracked_free(pool->free_slots, "SlabBitmap");
        if (pool->lock) vSemaphoreDelete(pool->lock);
        tracked_free(pool, "SlabPoolStruct");
        ESP_LOGE(SLAB_TAG, "Failed to create slab pool %s", name);
        return NULL;
    }
    for (int i = 0; i < num_items; i++) {
        pool->free_slots[i] = true;
    }
    ESP_LOGI(SLAB_TAG, "✅ Slab pool '%s' created: %d items, %d bytes/item (Total: %d bytes)", 
             name, num_items, item_size, total_mem_size);
    return pool;
}
void* slab_alloc(slab_pool_t* pool) {
    if (!pool) return NULL;
    if (xSemaphoreTake(pool->lock, pdMS_TO_TICKS(100)) == pdFALSE) return NULL;
    for (int i = 0; i < pool->total_items; i++) {
        if (pool->free_slots[i]) {
            pool->free_slots[i] = false;
            void* ptr = (uint8_t*)pool->memory_block + (i * pool->item_size);
            xSemaphoreGive(pool->lock);
            ESP_LOGI(SLAB_TAG, "Slab alloc from '%s', slot %d", pool->pool_name, i);
            return ptr;
        }
    }
    xSemaphoreGive(pool->lock);
    ESP_LOGW(SLAB_TAG, "Slab pool '%s' is full!", pool->pool_name);
    return NULL;
}
void slab_free(slab_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;
    int slot_index = -1;
    ptrdiff_t offset = (uint8_t*)ptr - (uint8_t*)pool->memory_block;
    if (offset >= 0 && (offset % pool->item_size == 0)) {
        slot_index = offset / pool->item_size;
        if (slot_index >= pool->total_items) {
            slot_index = -1;
        }
    }
    if (slot_index == -1) {
        ESP_LOGW(SLAB_TAG, "Attempted to free invalid pointer from slab '%s'", pool->pool_name);
        return;
    }
    if (xSemaphoreTake(pool->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (pool->free_slots[slot_index]) {
            ESP_LOGW(SLAB_TAG, "Slab free on already-freed slot %d!", slot_index);
        }
        pool->free_slots[slot_index] = true;
        ESP_LOGI(SLAB_TAG, "Slab free to '%s', slot %d", pool->pool_name, slot_index);
        xSemaphoreGive(pool->lock);
    }
}
// --- Shared Memory ---
typedef struct {
    char status_message[128];
    int update_counter;
    bool system_armed;
} shared_system_status_t;
static shared_system_status_t g_shared_status;
static SemaphoreHandle_t g_status_mutex;
void setup_shared_memory(void) {
    g_status_mutex = xSemaphoreCreateMutex();
    if (g_status_mutex) {
        xSemaphoreTake(g_status_mutex, portMAX_DELAY);
        strcpy(g_shared_status.status_message, "System Initializing...");
        g_shared_status.update_counter = 0;
        g_shared_status.system_armed = false;
        xSemaphoreGive(g_status_mutex);
    }
}
void update_system_status(const char* message) {
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(g_shared_status.status_message, message);
        g_shared_status.update_counter++;
        xSemaphoreGive(g_status_mutex);
        ESP_LOGI("SHARED_MEM", "Status updated!");
    } else {
        ESP_LOGW("SHARED_MEM", "Failed to get lock for status update");
    }
}
bool is_system_armed(void) {
    bool armed = false;
    if (xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        armed = g_shared_status.system_armed;
        xSemaphoreGive(g_status_mutex);
    }
    return armed;
}
// --- Memory analysis functions ---
void analyze_memory_status(void) {
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_free = esp_get_free_heap_size();
    float internal_fragmentation = 0.0;
    if (internal_free > 0) {
        internal_fragmentation = 1.0 - ((float)internal_largest / (float)internal_free);
    }
    ESP_LOGI(TAG, "\n📊 ═══ MEMORY STATUS ═══");
    ESP_LOGI(TAG, "Internal RAM Free:    %d bytes", internal_free);
    ESP_LOGI(TAG, "Largest Free Block:   %d bytes", internal_largest);
    ESP_LOGI(TAG, "SPIRAM Free:          %d bytes", spiram_free);
    ESP_LOGI(TAG, "Total Free:           %d bytes", total_free);
    ESP_LOGI(TAG, "Minimum Ever Free:    %d bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Internal Fragmentation: %.1f%%", internal_fragmentation * 100);
    if (internal_free < CRITICAL_MEMORY_THRESHOLD) {
        gpio_set_level(LED_MEMORY_ERROR, 1);
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_OK, 0);
        stats.low_memory_events++;
        ESP_LOGW(TAG, "🚨 CRITICAL: Very low memory!");
    } else if (internal_free < LOW_MEMORY_THRESHOLD) {
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_ERROR, 0);
        gpio_set_level(LED_MEMORY_OK, 0);
        stats.low_memory_events++;
        ESP_LOGW(TAG, "⚠️ WARNING: Low memory");
    } else {
        gpio_set_level(LED_MEMORY_OK, 1);
        gpio_set_level(LED_LOW_MEMORY, 0);
    }
    if (internal_fragmentation > FRAGMENTATION_THRESHOLD) {
        gpio_set_level(LED_FRAGMENTATION, 1);
        stats.fragmentation_events++;
        ESP_LOGW(TAG, "⚠️ High fragmentation detected!");
    } else {
        gpio_set_level(LED_FRAGMENTATION, 0);
    }
    if (spiram_free > 0) {
        gpio_set_level(LED_SPIRAM_ACTIVE, 1);
    } else {
        gpio_set_level(LED_SPIRAM_ACTIVE, 0);
    }
    ESP_LOGI(TAG, "═══════════════════════════════");
}
void print_allocation_summary(void) {
    if (!memory_mutex) return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ESP_LOGI(TAG, "\n📈 ═══ ALLOCATION STATISTICS ═══");
        ESP_LOGI(TAG, "Total Allocations:    %lu", stats.total_allocations);
        ESP_LOGI(TAG, "Total Deallocations:  %lu", stats.total_deallocations);
        ESP_LOGI(TAG, "Current Allocations:  %lu", stats.current_allocations);
        ESP_LOGI(TAG, "Total Allocated:      %llu bytes", stats.total_bytes_allocated);
        ESP_LOGI(TAG, "Total Deallocated:    %llu bytes", stats.total_bytes_deallocated);
        ESP_LOGI(TAG, "Peak Usage:           %llu bytes", stats.peak_usage);
        ESP_LOGI(TAG, "Allocation Failures:  %lu", stats.allocation_failures);
        ESP_LOGI(TAG, "Fragmentation Events: %lu", stats.fragmentation_events);
        ESP_LOGI(TAG, "Low Memory Events:    %lu", stats.low_memory_events);
        if (stats.current_allocations > 0) {
            ESP_LOGI(TAG, "\n🔍 ═══ ACTIVE ALLOCATIONS ═══");
            for (int i = 0; i < MAX_ALLOCATIONS; i++) {
                if (allocations[i].is_active) {
                    uint64_t age_ms = (esp_timer_get_time() - allocations[i].timestamp) / 1000;
                    ESP_LOGI(TAG, "Slot %d: %d bytes at %p (%s) - Age: %llu ms",
                             i, allocations[i].size, allocations[i].ptr,
                             allocations[i].description, age_ms);
                }
            }
        }
        xSemaphoreGive(memory_mutex);
    }
}
void analyze_allocation_patterns(void) {
    if (!memory_mutex) return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        size_t internal_usage = 0;
        size_t spiram_usage = 0;
        size_t dma_usage = 0;
        int internal_count = 0;
        int spiram_count = 0;
        int dma_count = 0;
        for (int i = 0; i < MAX_ALLOCATIONS; i++) {
            if (allocations[i].is_active) {
                if (allocations[i].caps & MALLOC_CAP_INTERNAL) {
                    internal_usage += allocations[i].size;
                    internal_count++;
                } else if (allocations[i].caps & MALLOC_CAP_SPIRAM) {
                    spiram_usage += allocations[i].size;
                    spiram_count++;
                }
                if (allocations[i].caps & MALLOC_CAP_DMA) {
                    dma_usage += allocations[i].size;
                    dma_count++;
                }
            }
        }
        ESP_LOGI(TAG, "\n📊 Allocation Patterns:");
        ESP_LOGI(TAG, "Internal: %d bytes in %d allocations", internal_usage, internal_count);
        ESP_LOGI(TAG, "SPIRAM: %d bytes in %d allocations", spiram_usage, spiram_count);
        ESP_LOGI(TAG, "DMA: %d bytes in %d allocations", dma_usage, dma_count);
        xSemaphoreGive(memory_mutex);
    }
}
void detect_memory_leaks(void) {
    if (!memory_mutex) return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        uint64_t current_time = esp_timer_get_time();
        int leak_count = 0;
        size_t leaked_bytes = 0;
        ESP_LOGI(TAG, "\n🔍 ═══ MEMORY LEAK DETECTION ═══");
        for (int i = 0; i < MAX_ALLOCATIONS; i++) {
            if (allocations[i].is_active) {
                uint64_t age_ms = (current_time - allocations[i].timestamp) / 1000;
                if (age_ms > 30000) {
                    ESP_LOGW(TAG, "POTENTIAL LEAK: %d bytes at %p (%s) - Age: %llu ms",
                             allocations[i].size, allocations[i].ptr,
                             allocations[i].description, age_ms);
                    leak_count++;
                    leaked_bytes += allocations[i].size;
                }
            }
        }
        if (leak_count > 0) {
            ESP_LOGW(TAG, "Found %d potential leaks totaling %d bytes", leak_count, leaked_bytes);
            gpio_set_level(LED_MEMORY_ERROR, 1);
        } else {
            ESP_LOGI(TAG, "No memory leaks detected");
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > LOW_MEMORY_THRESHOLD) {
                 gpio_set_level(LED_MEMORY_ERROR, 0);
            }
        }
        xSemaphoreGive(memory_mutex);
    }
}
// --- Test tasks ---
void memory_stress_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "🧪 Memory stress test started (Sequential Mode)");
    void* test_ptrs[20] = {NULL};
    while (1) {
        ESP_LOGI(TAG, "🔧 Stress test: Allocating 20 blocks...");
        int allocation_count = 0;
        for (int i = 0; i < 20; i++) {
            size_t size = 100 + (esp_random() % 2000);
            uint32_t caps = (esp_random() % 2) ? MALLOC_CAP_INTERNAL : MALLOC_CAP_DEFAULT;
            test_ptrs[i] = tracked_malloc(size, caps, "StressTest");
            if (test_ptrs[i]) {
                memset(test_ptrs[i], 0xAA, size);
                allocation_count++;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGI(TAG, "🔧 Stress test: Allocated %d blocks. Holding...", allocation_count);
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "🗑️ Stress test: Freeing %d blocks...", allocation_count);
        for (int i = 0; i < 20; i++) {
            if (test_ptrs[i]) {
                tracked_free(test_ptrs[i], "StressTest");
                test_ptrs[i] = NULL;
            }
        }
        ESP_LOGI(TAG, "🗑️ Stress test: All blocks freed. Waiting...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
void memory_pool_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏊 Memory pool test started");
    const size_t pool_sizes[] = {64, 128, 256, 512, 1024};
    const int num_pools = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
    void* pools[5][10] = {NULL};
    while (1) {
        ESP_LOGI(TAG, "🏊 Allocating memory pools...");
        for (int size_idx = 0; size_idx < num_pools; size_idx++) {
            for (int i = 0; i < 10; i++) {
                char desc[32];
                snprintf(desc, sizeof(desc), "Pool%d_%d", size_idx, i);
                pools[size_idx][i] = tracked_malloc(pool_sizes[size_idx], 
                                                    MALLOC_CAP_INTERNAL, desc);
                if (pools[size_idx][i]) {
                    memset(pools[size_idx][i], 0x55 + size_idx, pool_sizes[size_idx]);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "🏊 Freeing memory pools (reverse order)...");
        for (int size_idx = num_pools - 1; size_idx >= 0; size_idx--) {
            for (int i = 9; i >= 0; i--) {
                if (pools[size_idx][i]) {
                    tracked_free(pools[size_idx][i], "Pool");
                    pools[size_idx][i] = NULL;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}
void large_allocation_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "🐘 Large allocation test started");
    while (1) {
        size_t large_size = 50000 + (esp_random() % 100000);
        ESP_LOGI(TAG, "🐘 Attempting large allocation: %d bytes", large_size);
        void* large_ptr = tracked_malloc(large_size, MALLOC_CAP_INTERNAL, "LargeInternal");
        if (!large_ptr) {
            ESP_LOGW(TAG, "🐘 Internal RAM failed, trying SPIRAM...");
            large_ptr = tracked_malloc(large_size, MALLOC_CAP_SPIRAM, "LargeSPIRAM");
        }
        if (large_ptr) {
            ESP_LOGI(TAG, "🐘 Large allocation successful: %p", large_ptr);
            uint64_t start_time = esp_timer_get_time();
            memset(large_ptr, 0xFF, large_size);
            uint64_t end_time = esp_timer_get_time();
            uint32_t access_time_ms = (end_time - start_time) / 1000;
            ESP_LOGI(TAG, "🐘 Memory access time: %lu ms", access_time_ms);
            vTaskDelay(pdMS_TO_TICKS(10000));
            tracked_free(large_ptr, "Large");
        } else {
            ESP_LOGE(TAG, "🐘 Large allocation failed!");
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
void memory_batch_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "📦 Batch allocation test started");
    while (1) {
        ESP_LOGI(TAG, "📦 Creating 'SensorData' batch...");
        memory_batch_t* sensor_batch = create_memory_batch("SensorDataBatch");
        if (sensor_batch) {
            batch_allocate(sensor_batch, 1024, MALLOC_CAP_INTERNAL);
            batch_allocate(sensor_batch, 512, MALLOC_CAP_INTERNAL);
            batch_allocate(sensor_batch, 256, MALLOC_CAP_DMA);
            ESP_LOGI(TAG, "📦 Batch 'SensorData' allocated with %d items", sensor_batch->count);
            vTaskDelay(pdMS_TO_TICKS(7000));
            ESP_LOGI(TAG, "📦 Freeing 'SensorData' batch...");
            batch_free(sensor_batch);
        } else {
            ESP_LOGE(TAG, "📦 Failed to create memory batch!");
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
// --- New Test Task (Slab) ---
void slab_allocator_test_task(void *pvParameters) {
    ESP_LOGI(SLAB_TAG, "🧪 Slab allocator test task started");
    slab_pool_t* sensor_pool = slab_pool_create("SensorPool", sizeof(sensor_data_t), 20);
    if (!sensor_pool) {
        ESP_LOGE(SLAB_TAG, "Failed to create sensor pool!");
        vTaskDelete(NULL);
        return;
    }
    void* allocations[20] = {NULL};
    int count = 0;
    while(1) {
        ESP_LOGI(SLAB_TAG, "Slab test: Allocating 5 items...");
        for(int i=0; i < 5; i++) {
            if (count < 20) {
                allocations[count] = slab_alloc(sensor_pool);
                if (allocations[count]) {
                    sensor_data_t* data = (sensor_data_t*)allocations[count];
                    data->temperature = 25.0 + count;
                    data->timestamp = esp_timer_get_time();
                    count++;
                } else {
                    ESP_LOGW(SLAB_TAG, "Slab pool full! Count = %d", count);
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4000));
        ESP_LOGI(SLAB_TAG, "Slab test: Freeing 2 items...");
        for(int i=0; i < 2; i++) {
            if (count > 0) {
                count--;
                slab_free(sensor_pool, allocations[count]);
                allocations[count] = NULL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
// --- New Test Tasks (Shared Mem) ---
void task_A_shared_mem(void* pv) {
    ESP_LOGI("SHARED_MEM", "Task A started");
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        update_system_status("Task A reporting in");
    }
}
void task_B_shared_mem(void* pv) {
    ESP_LOGI("SHARED_MEM", "Task B started");
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (is_system_armed()) {
            ESP_LOGW("SHARED_MEM", "Task B: System is ARMED!");
        } else {
            ESP_LOGI("SHARED_MEM", "Task B: System is disarmed.");
        }
    }
}
// --- Monitor Tasks ---
void memory_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "📊 Memory monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        analyze_memory_status();
        print_allocation_summary();
        analyze_allocation_patterns();
        detect_memory_leaks();
        if (!heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "🚨 HEAP CORRUPTION DETECTED!");
            gpio_set_level(LED_MEMORY_ERROR, 1);
        }
        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "System uptime: %llu ms\n", esp_timer_get_time() / 1000);
    }
}
void heap_integrity_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "🔍 Heap integrity test started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "🔍 Running heap integrity check...");
        bool integrity_ok = heap_caps_check_integrity_all(true);
        if (integrity_ok) {
            ESP_LOGI(TAG, "✅ Heap integrity OK");
        } else {
            ESP_LOGE(TAG, "❌ Heap integrity check FAILED!");
            gpio_set_level(LED_MEMORY_ERROR, 1);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
                heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
            }
        }
        ESP_LOGI(TAG, "🔍 Running memory performance test...");
        const size_t test_size = 4096;
        void* test_buf = tracked_malloc(test_size, MALLOC_CAP_INTERNAL, "PerfTest");
        if (test_buf) {
            uint64_t start = esp_timer_get_time();
            for (int i = 0; i < 100; i++) {
                memset(test_buf, i & 0xFF, test_size);
            }
            uint64_t write_time = esp_timer_get_time() - start;
            start = esp_timer_get_time();
            volatile uint8_t checksum = 0;
            for (int i = 0; i < 100; i++) {
                uint8_t* buf = (uint8_t*)test_buf;
                for (size_t j = 0; j < test_size; j++) {
                    checksum += buf[j];
                }
            }
            uint64_t read_time = esp_timer_get_time() - start;
            ESP_LOGI(TAG, "🔍 Performance: Write %llu μs, Read %llu μs", 
                     write_time, read_time);
            tracked_free(test_buf, "PerfTest");
        }
    }
}
// --- Main ---
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Heap Management Lab Starting...");
    gpio_set_direction(LED_MEMORY_OK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LOW_MEMORY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEMORY_ERROR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_FRAGMENTATION, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SPIRAM_ACTIVE, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_MEMORY_OK, 0);
    gpio_set_level(LED_LOW_MEMORY, 0);
    gpio_set_level(LED_MEMORY_ERROR, 0);
    gpio_set_level(LED_FRAGMENTATION, 0);
    gpio_set_level(LED_SPIRAM_ACTIVE, 0);
    memory_mutex = xSemaphoreCreateMutex();
    if (!memory_mutex) {
        ESP_LOGE(TAG, "Failed to create memory mutex!");
        return;
    }
    memset(allocations, 0, sizeof(allocations));
    setup_shared_memory();
    ESP_LOGI(TAG, "Memory tracking system initialized");
    analyze_memory_status();
    ESP_LOGI(TAG, "\n🏗️ ═══ INITIAL HEAP INFORMATION ═══");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
        ESP_LOGI(TAG, "\n🏗️ ═══ SPIRAM INFORMATION ═══");
        heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    }
    ESP_LOGI(TAG, "Creating memory test tasks...");
    // <<< แก้ไข: เพิ่มขนาด Stack ให้ Task ทั้งหมด
    xTaskCreate(memory_monitor_task, "MemMonitor", 4096, NULL, 6, NULL);
    xTaskCreate(memory_stress_test_task, "StressTest", 4096, NULL, 5, NULL); // 3072 -> 4096
    xTaskCreate(memory_pool_test_task, "PoolTest", 4096, NULL, 5, NULL); // 3072 -> 4096
    xTaskCreate(large_allocation_test_task, "LargeAlloc", 3072, NULL, 4, NULL); // 2048 -> 3072
    xTaskCreate(heap_integrity_test_task, "IntegrityTest", 4096, NULL, 3, NULL); // 3072 -> 4096
    xTaskCreate(memory_batch_test_task, "BatchTest", 4096, NULL, 4, NULL); // 3072 -> 4096
    xTaskCreate(slab_allocator_test_task, "SlabTest", 4096, NULL, 4, NULL); // 3072 -> 4096
    xTaskCreate(task_A_shared_mem, "SharedMemA", 3072, NULL, 3, NULL); // 2048 -> 3072
    xTaskCreate(task_B_shared_mem, "SharedMemB", 3072, NULL, 3, NULL); // 2048 -> 3072
    ESP_LOGI(TAG, "All tasks created successfully");
    ESP_LOGI(TAG, "\n🎯 LED Indicators:");
    ESP_LOGI(TAG, "  GPIO2  - Memory System OK (Green)");
    ESP_LOGI(TAG, "  GPIO4  - Low Memory Warning (Yellow)");
    ESP_LOGI(TAG, "  GPIO5  - Memory Error/Leak (Red)");
    ESP_LOGI(TAG, "  GPIO18 - High Fragmentation (Orange)");
    ESP_LOGI(TAG, "  GPIO19 - SPIRAM Active (Blue)");
    ESP_LOGI(TAG, "\n🔬 Test Features:");
    ESP_LOGI(TAG, "  • Dynamic Memory Allocation Tracking");
    ESP_LOGI(TAG, "  • Real-time Memory Status Monitoring");
    ESP_LOGI(TAG, "  • Memory Leak Detection");
    ESP_LOGI(TAG, "  • Fragmentation Analysis");
    ESP_LOGI(TAG, "  • Heap Integrity Checking");
    ESP_LOGI(TAG, "  • Memory Performance Testing");
    ESP_LOGI(TAG, "  • Batch Allocation Management");
    ESP_LOGI(TAG, "  • Slab Allocator (Custom Pool)");
    ESP_LOGI(TAG, "  • Shared Memory (Mutex-protected)");
    ESP_LOGI(TAG, "Heap Management System operational!");
}
