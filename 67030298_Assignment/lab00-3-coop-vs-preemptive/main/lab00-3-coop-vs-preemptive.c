#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

// --- Pin Definitions ---
#define LED1_PIN GPIO_NUM_2   // Task 1 indicator
#define LED2_PIN GPIO_NUM_4   // Task 2 indicator
#define LED3_PIN GPIO_NUM_5   // Task 3 indicator / Emergency
#define BUTTON_PIN GPIO_NUM_0 // Emergency button

// --- Tags for Logging ---
static const char *TAG_COOP = "COOPERATIVE";
static const char *TAG_PREEMPT = "PREEMPTIVE";
static const char *TAG_MAIN = "MAIN";

// --- Global variables for Cooperative Scheduling ---
static volatile bool coop_emergency_flag = false;
static uint64_t coop_task_start_time = 0;
static uint32_t coop_max_response_time = 0;

// --- Task structures for Cooperative Multitasking ---
typedef struct {
    void (*task_function)(void);
    const char* name;
    bool ready;
} coop_task_t;

// --- Cooperative Task Implementations ---
void cooperative_task1(void)
{
    static uint32_t count = 0;
    ESP_LOGI(TAG_COOP, "Coop Task1 running: %lu", count++);

    gpio_set_level(LED1_PIN, 1);

    // Simulate work with voluntary yielding
    for (int i = 0; i < 5; i++) {
        // Do some work
        for (int j = 0; j < 50000; j++) {
            volatile int dummy = j * 2;
        }

        // Check for emergency and yield if needed
        if (coop_emergency_flag) {
            ESP_LOGW(TAG_COOP, "Task1 yielding for emergency");
            gpio_set_level(LED1_PIN, 0);
            // Simulate context save time before yielding
            for(int k=0; k<10000; k++) {volatile int d=k;}
            vTaskDelay(1); // Yield immediately
            return;
        }

        // Voluntary yield point
        // Simulate context save time before yielding
        for(int k=0; k<10000; k++) {volatile int d=k;}
        vTaskDelay(1); // Minimal delay to yield
    }

    gpio_set_level(LED1_PIN, 0);
    // Simulate context save time before yielding
    for(int k=0; k<10000; k++) {volatile int d=k;}
    vTaskDelay(1); // Ensure yield at the end
}

void cooperative_task2(void)
{
    static uint32_t count = 0;
    ESP_LOGI(TAG_COOP, "Coop Task2 running: %lu", count++);

    gpio_set_level(LED2_PIN, 1);

    // Simulate longer work with yielding
    for (int i = 0; i < 10; i++) { // Increased loop count for longer work simulation
        for (int j = 0; j < 30000; j++) {
            volatile int dummy = j + i;
        }

        if (coop_emergency_flag) {
            ESP_LOGW(TAG_COOP, "Task2 yielding for emergency");
            gpio_set_level(LED2_PIN, 0);
            // Simulate context save time before yielding
            for(int k=0; k<10000; k++) {volatile int d=k;}
            vTaskDelay(1);
            return;
        }

        // Simulate context save time before yielding
        for(int k=0; k<10000; k++) {volatile int d=k;}
        vTaskDelay(1);
    }

    gpio_set_level(LED2_PIN, 0);
    // Simulate context save time before yielding
     for(int k=0; k<10000; k++) {volatile int d=k;}
    vTaskDelay(1); // Ensure yield at the end
}

void cooperative_task3_emergency(void)
{
    // This task specifically checks and handles the emergency flag
    if (coop_emergency_flag) {
        uint64_t response_time = esp_timer_get_time() - coop_task_start_time;
        uint32_t response_ms = (uint32_t)(response_time / 1000);

        if (response_ms > coop_max_response_time) {
            coop_max_response_time = response_ms;
        }

        ESP_LOGW(TAG_COOP, "EMERGENCY RESPONSE! Response time: %lu ms (Max: %lu ms)",
                 response_ms, coop_max_response_time);

        gpio_set_level(LED3_PIN, 1);
        // Use a simple busy loop for delay in cooperative context if vTaskDelay causes issues
        uint64_t start = esp_timer_get_time();
        while(esp_timer_get_time() - start < 200000) {}; // 200ms busy wait
        gpio_set_level(LED3_PIN, 0);

        coop_emergency_flag = false; // Reset flag after handling
    }
    // Simulate context save time before yielding
    for(int k=0; k<10000; k++) {volatile int d=k;}
     vTaskDelay(1); // Yield even if no emergency
}

// --- Cooperative Scheduler ---
void cooperative_scheduler(void)
{
    coop_task_t tasks[] = {
        {cooperative_task1, "Task1", true},
        {cooperative_task2, "Task2", true},
        {cooperative_task3_emergency, "EmergencyCheck", true} // Task dedicated to checking emergency
    };

    int num_tasks = sizeof(tasks) / sizeof(tasks[0]);
    int current_task = 0;

    ESP_LOGI(TAG_MAIN, "Starting Cooperative Scheduler");

    while (1) {
        // Check for emergency button outside the task loop for faster detection
        if (gpio_get_level(BUTTON_PIN) == 0 && !coop_emergency_flag) {
            coop_emergency_flag = true;
            coop_task_start_time = esp_timer_get_time();
            ESP_LOGW(TAG_COOP, "!!! Emergency button pressed !!!");
        }

        // Simulate context load time
        for(int k=0; k<10000; k++) {volatile int d=k;}

        // Run current task if ready
        if (tasks[current_task].ready) {
             ESP_LOGD(TAG_COOP, "Running %s", tasks[current_task].name);
            tasks[current_task].task_function();
        }

        // Move to next task
        current_task = (current_task + 1) % num_tasks;

        // Small delay in scheduler loop is NOT cooperative yielding, tasks must yield themselves.
        // vTaskDelay(pdMS_TO_TICKS(10)); // Remove this delay or keep it very small (1 tick) just for watchdog
        vTaskDelay(1); // minimal delay to allow button check and prevent busy loop in scheduler itself
    }
}

void test_cooperative_multitasking(void)
{
    ESP_LOGI(TAG_MAIN, "=== Cooperative Multitasking Demo ===");
    ESP_LOGI(TAG_MAIN, "Tasks will yield voluntarily");
    ESP_LOGI(TAG_MAIN, "Press button (GPIO0) to test emergency response");

    // We run the cooperative scheduler in the main task context
    cooperative_scheduler();
}


// --- Preemptive Multitasking using FreeRTOS ---
static volatile bool preempt_emergency_flag = false;
static uint64_t preempt_start_time = 0;
static uint32_t preempt_max_response = 0;

void preemptive_task1(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(TAG_PREEMPT, "Preempt Task1: %lu", count++);

        gpio_set_level(LED1_PIN, 1);

        // Simulate work WITHOUT explicit yielding
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 50000; j++) {
                volatile int dummy = j * 2;
            }
            // RTOS can preempt here if a higher priority task becomes ready
            // or if time slicing is enabled and the time slice expires.
        }

        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100)); // Block, allowing lower priority tasks to run
    }
}

void preemptive_task2(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(TAG_PREEMPT, "Preempt Task2: %lu", count++);

        gpio_set_level(LED2_PIN, 1);

        // Simulate LONGER work without explicit yielding
        for (int i = 0; i < 10; i++) { // Reduced loop slightly from README example for balance
            for (int j = 0; j < 30000; j++) {
                volatile int dummy = j + i;
            }
             // RTOS can preempt here
        }

        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150)); // Block
    }
}

void preemptive_emergency_task(void *pvParameters)
{
    ESP_LOGI(TAG_PREEMPT, "Emergency Task Started (High Priority)");
    while (1) {
        // Check button state
        if (gpio_get_level(BUTTON_PIN) == 0) {
             if (!preempt_emergency_flag) { // Check if flag is not already set
                preempt_emergency_flag = true;
                preempt_start_time = esp_timer_get_time(); // Record time when pressed

                // Since this task has the highest priority, it preempts others immediately.
                uint64_t response_time = esp_timer_get_time() - preempt_start_time; // Response time is almost immediate
                uint32_t response_ms = (uint32_t)(response_time / 1000);

                if (response_ms > preempt_max_response) {
                    preempt_max_response = response_ms;
                }

                ESP_LOGW(TAG_PREEMPT, "!!! IMMEDIATE EMERGENCY RESPONSE !!! Response: %lu ms (Max: %lu ms)",
                         response_ms, preempt_max_response);

                // Indicate emergency
                gpio_set_level(LED3_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(200)); // Show LED for 200ms
                gpio_set_level(LED3_PIN, 0);

                 // Wait for button release before resetting the flag
                 ESP_LOGI(TAG_PREEMPT, "Waiting for button release...");
                 while(gpio_get_level(BUTTON_PIN) == 0) {
                     vTaskDelay(pdMS_TO_TICKS(10));
                 }
                 ESP_LOGI(TAG_PREEMPT, "Button released.");
                 preempt_emergency_flag = false; // Reset flag only after release
            }
        } else {
             // Optional: If you want to reset the flag immediately when the button is released
             // preempt_emergency_flag = false;
        }

        // Check button frequently
        vTaskDelay(pdMS_TO_TICKS(10)); // Check every 10ms
    }
}


void test_preemptive_multitasking(void)
{
    ESP_LOGI(TAG_MAIN, "=== Preemptive Multitasking Demo ===");
    ESP_LOGI(TAG_MAIN, "RTOS will preempt tasks automatically based on priority");
    ESP_LOGI(TAG_MAIN, "Press button (GPIO0) to test emergency response");

    // Create tasks with different priorities
    // Higher number means higher priority
    xTaskCreate(preemptive_task1, "PreTask1", 2048, NULL, 2, NULL);      // Normal priority
    xTaskCreate(preemptive_task2, "PreTask2", 2048, NULL, 1, NULL);      // Low priority (will run less often)
    xTaskCreate(preemptive_emergency_task, "Emergency", 2048, NULL, 5, NULL); // High priority (will preempt others)

    ESP_LOGI(TAG_MAIN, "Preemptive tasks created. Main task exiting.");
    // No need for the main task anymore in preemptive mode if it doesn't do anything else.
    // vTaskDelete(NULL); // Optional: Delete the app_main task
}


void app_main(void)
{
    // --- GPIO Configuration (เหมือนเดิม) ---
    // Output LEDs
    gpio_config_t io_conf_output = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf_output);

    // Input Button with Pull-up
    gpio_config_t io_conf_input = {
        .intr_type = GPIO_INTR_DISABLE, // Interrupts not used in this example
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE, // Use internal pull-up resistor
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf_input);

    ESP_LOGI(TAG_MAIN, "Multitasking Comparison Demo Initialized");
    ESP_LOGI(TAG_MAIN, "Choose test mode by uncommenting the desired function call below:");

    // --- เลือกโหมดทดสอบ ---
    // Uncomment ONE of the following lines:

    // test_cooperative_multitasking();
    test_preemptive_multitasking();

    ESP_LOGI(TAG_MAIN, "App main finished setup.");
     // If test_preemptive_multitasking is called, app_main might finish
     // while the created tasks continue running.
     // If test_cooperative_multitasking is called, it contains an infinite loop,
     // so code below this line won't execute in that mode.
}
