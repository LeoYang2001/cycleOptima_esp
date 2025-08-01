#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include "main.h"

#define NUM_COMPONENTS 8
#define MAX_TASK_NAME_LEN 32

typedef struct {
    const char* name;
    gpio_num_t  pin;
    bool        is_active;            // not strictly needed here, but kept for reference
} ComponentState;

ComponentState component_states[NUM_COMPONENTS] = {
    {"Retractor", RETRACTOR_PIN, 0, false},
    {"Drain Valve", DRAIN_VALVE_PIN, 0, false},
    {"Cold Valve", COLD_VALVE_PIN, 0, false},
    {"Drain Pump", DRAIN_PUMP_PIN, 0, false},
    {"Hot Valve", HOT_VALVE_PIN, 0, false},
    {"Softener Valve", SOFT_VALVE_PIN, 0, false},
    {"Motor", MOTOR_ON_PIN, 0, false},
    {"Motor Direction", MOTOR_DIRECTION_PIN, 0, false},
};

gpio_num_t map_name_to_pin(const char* name) {
    for (int i = 0; i < NUM_COMPONENTS; i++) {
        if (strcmp(component_states[i].name, name) == 0) {
            return component_states[i].pin;
        }
    }
    return -1;
}


typedef struct {
    uint32_t    stepTime;         // Time for each step in milliseconds
    const char* direction;        // "cw" (clockwise) or "ccw" (counter-clockwise)
    uint32_t    pauseTime;        // Pause time between steps in milliseconds
} MotorPattern;

typedef struct {
    MotorPattern* pattern;        // Array of motor patterns
    int           pattern_count;  // Number of patterns in the array
    uint32_t      repeatTimes;    // Number of times to repeat the pattern
    const char*   runningStyle;   // "Single Direction", "Alternating", etc.
} MotorConfig;

typedef struct {
    const char* compId;
    gpio_num_t  pin;
    uint32_t    start;    // ms delay from phase start before running
    uint32_t    duration;         // how long (ms) to run this component
    uint32_t    stepTime;         // used for motor styles (deprecated - use motorConfig)
    const char* runningStyle;     // "toggle" or "singleDir" (deprecated - use motorConfig)
    uint32_t    pauseTime;        // only used if runningStyle == "singleDir" (deprecated)
    MotorConfig* motorConfig;     // NULL for non-motor components, or motor configuration
} ComponentInput;

typedef struct {
    const char*     name;
    uint32_t        startTime;   // ms delay from “0” (or from last phase) to start this phase
    ComponentInput* components;
    int             num_components;
} Phase;

Phase* program_phases = NULL;
int NUM_PHASES = 0;

// Timer logger variables
static uint32_t program_start_time = 0;
static bool timer_logger_running = false;

uint32_t get_millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t get_program_elapsed_ms() {
    if (program_start_time == 0) return 0;
    return get_millis() - program_start_time;
}

// Timer logger task - runs every 1000ms and prints elapsed time
static void timer_logger_task(void* arg) {
    while (timer_logger_running) {
        uint32_t elapsed = get_program_elapsed_ms();
        ESP_LOGI("TIMER", "Program elapsed time: %lu ms (%lu.%03lu seconds)",
                 (unsigned long)elapsed,
                 (unsigned long)(elapsed / 1000),
                 (unsigned long)(elapsed % 1000));
        vTaskDelay(pdMS_TO_TICKS(1000));  // Log every 1000ms
    }
    vTaskDelete(NULL);
}

typedef struct {
    const char* compId;
    gpio_num_t  pin;
    uint32_t    start;
    uint32_t    duration;
    MotorConfig* motorConfig;  // Add motor configuration to task args
} ComponentTaskArg;



// --------------------------------------------------
// New function: all motor‐running logic goes here
// --------------------------------------------------
static void run_motor_task(const ComponentTaskArg* c) {
    ESP_LOGI("COMPONENT_TASK", "Running motor task for %s", c->compId);

    if (!c->motorConfig) {
        ESP_LOGW("COMPONENT_TASK", "No motor config for %s, treating as regular component", c->compId);
        return;
    }

    gpio_num_t motor_on_pin = c->pin;  // Motor ON/OFF pin
    gpio_num_t motor_dir_pin = MOTOR_DIRECTION_PIN;  // Motor direction pin

    // Turn motor ON
    gpio_set_level(motor_on_pin, 0);  // Active LOW
    ESP_LOGI("COMPONENT_TASK", "Motor %s started at %lu ms", c->compId, (unsigned long)get_millis());

    uint32_t total_runtime = 0;
    uint32_t target_duration = c->duration;

    for (uint32_t repeat = 0; repeat < c->motorConfig->repeatTimes && total_runtime < target_duration; repeat++) {
        ESP_LOGI("COMPONENT_TASK", "Motor %s - Repeat cycle %lu/%lu", 
                 c->compId, (unsigned long)(repeat + 1), (unsigned long)c->motorConfig->repeatTimes);

        for (int i = 0; i < c->motorConfig->pattern_count && total_runtime < target_duration; i++) {
            MotorPattern* pattern = &c->motorConfig->pattern[i];
            
            // Set motor direction
            bool clockwise = (strcmp(pattern->direction, "cw") == 0);
            gpio_set_level(motor_dir_pin, clockwise ? 0 : 1);//testing
            
            ESP_LOGI("COMPONENT_TASK", "Motor %s - Step %d: %s for %lu ms", 
                     c->compId, i + 1, pattern->direction, (unsigned long)pattern->stepTime);

            // Run motor for stepTime
            uint32_t step_duration = (pattern->stepTime + total_runtime > target_duration) ? 
                                   (target_duration - total_runtime) : pattern->stepTime;
            vTaskDelay(pdMS_TO_TICKS(step_duration));
            total_runtime += step_duration;

            if (total_runtime >= target_duration) break;

            // Pause if specified and we haven't reached duration
            if (pattern->pauseTime > 0) {
                // Turn motor OFF during pause
                gpio_set_level(motor_on_pin, 1);  // Active LOW, so 1 = OFF
                ESP_LOGI("COMPONENT_TASK", "Motor %s - Pausing (motor OFF) for %lu ms", 
                         c->compId, (unsigned long)pattern->pauseTime);
                
                uint32_t pause_duration = (pattern->pauseTime + total_runtime > target_duration) ? 
                                        (target_duration - total_runtime) : pattern->pauseTime;
                vTaskDelay(pdMS_TO_TICKS(pause_duration));
                total_runtime += pause_duration;
                
                // Turn motor back ON for next step (if we haven't exceeded duration)
                if (total_runtime < target_duration) {
                    gpio_set_level(motor_on_pin, 0);  // Active LOW, so 0 = ON
                    ESP_LOGI("COMPONENT_TASK", "Motor %s - Resuming (motor ON)", c->compId);
                }
            }
        }
    }

    // Turn motor OFF
    gpio_set_level(motor_on_pin, 1);  // Active LOW, so 1 = OFF
    ESP_LOGI("COMPONENT_TASK", "Motor %s stopped at %lu ms (ran for %lu ms)", 
             c->compId, (unsigned long)get_millis(), (unsigned long)total_runtime);
}
static void component_task(void* arg) {
    ComponentTaskArg* c = (ComponentTaskArg*)arg;

    // 1) Wait until it’s time to start
    if (c->start > 0) {
        vTaskDelay(pdMS_TO_TICKS(c->start));
    }

    // Check if this is a motor component with motor configuration
    if (c->motorConfig != NULL) {
        // Delegate all motor logic to run_motor_task()
        run_motor_task(c);
    } else {
        // Non‐motor: simple ON for duration, then OFF
        gpio_set_level(c->pin, 0);  // ON
        ESP_LOGI("COMPONENT_TASK",
                 "Component %s ON at %lu ms for %lu ms",
                 c->compId, (unsigned long)get_millis(), (unsigned long)c->duration);
        vTaskDelay(pdMS_TO_TICKS(c->duration));
        gpio_set_level(c->pin, 1);  // OFF
        ESP_LOGI("COMPONENT_TASK",
                 "Component %s OFF at %lu ms",
                 c->compId, (unsigned long)get_millis());
    }

    // Clean up and delete this task
    free(c);
    vTaskDelete(NULL);
}

static void run_phase(const Phase* phase) {
    // 5a) Compute each component’s absolute (relative to phase) start and end:
    uint32_t phase_duration = 0;
    for (int i = 0; i < phase->num_components; i++) {
        const ComponentInput* comp = &phase->components[i];
        uint32_t finish_time = comp->start + comp->duration;
        if (finish_time > phase_duration) {
            phase_duration = finish_time;
        }

        // Build argument struct for the component task
        ComponentTaskArg* arg = malloc(sizeof(ComponentTaskArg));
        *arg = (ComponentTaskArg){
            .compId         = comp->compId,
            .pin            = comp->pin,
            .start  = comp->start,
            .duration       = comp->duration,
            .motorConfig    = comp->motorConfig,  // Pass motor configuration
        };

        // Name each task after the component for easier debugging:
        char task_name[MAX_TASK_NAME_LEN];
        snprintf(task_name, MAX_TASK_NAME_LEN, "comp_%s", comp->compId);

        // Create the FreeRTOS task:
        xTaskCreate(
            component_task,
            task_name,
            4096,        // stack size; adjust as needed
            arg,
            tskIDLE_PRIORITY + 1,
            NULL
        );
    }

    // 5b) Wait until the longest component in this phase is done:
    //     Since each task self‐deletes exactly when its work is done,
    //     we simply delay for the full phase duration plus a small buffer.
    vTaskDelay(pdMS_TO_TICKS(phase_duration + 50));
}

static bool load_json_config(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        ESP_LOGE("CONFIG", "Failed to open %s", path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long len = ftell(file);
    rewind(file);

    char* content = malloc(len + 1);
    fread(content, 1, len, file);
    content[len] = '\0';
    fclose(file);

    cJSON* root = cJSON_Parse(content);
    if (!root) {
        ESP_LOGE("CONFIG", "Failed to parse JSON");
        free(content);
        return false;
    }

    NUM_PHASES = cJSON_GetArraySize(root);
    program_phases = calloc(NUM_PHASES, sizeof(Phase));

    for (int i = 0; i < NUM_PHASES; i++) {
        cJSON* phaseObj = cJSON_GetArrayItem(root, i);
        program_phases[i].name = strdup(cJSON_GetObjectItem(phaseObj, "name")->valuestring);
        program_phases[i].startTime = cJSON_GetObjectItem(phaseObj, "startTime")->valueint;

        cJSON* comps = cJSON_GetObjectItem(phaseObj, "components");
        int num_comps = cJSON_GetArraySize(comps);
        program_phases[i].components = calloc(num_comps, sizeof(ComponentInput));
        program_phases[i].num_components = num_comps;

        // Track the maximum (start + duration) so we know phase duration:
        uint32_t max_phase_dur = 0;

        for (int j = 0; j < num_comps; j++) {
            cJSON* compObj = cJSON_GetArrayItem(comps, j);
            ComponentInput* dst = &program_phases[i].components[j];

            const char* name = cJSON_GetObjectItem(compObj, "compId")->valuestring;
            dst->compId = strdup(name);
            dst->pin    = map_name_to_pin(name);
            dst->start = cJSON_GetObjectItem(compObj, "start")->valueint;
            dst->duration      = cJSON_GetObjectItem(compObj, "duration")->valueint;

            // Parse motorConfig if present
            cJSON* motorConfigObj = cJSON_GetObjectItem(compObj, "motorConfig");
            if (motorConfigObj && !cJSON_IsNull(motorConfigObj)) {
                dst->motorConfig = malloc(sizeof(MotorConfig));
                
                // Parse pattern array
                cJSON* patternArray = cJSON_GetObjectItem(motorConfigObj, "pattern");
                if (patternArray) {
                    int pattern_count = cJSON_GetArraySize(patternArray);
                    dst->motorConfig->pattern = malloc(pattern_count * sizeof(MotorPattern));
                    dst->motorConfig->pattern_count = pattern_count;
                    
                    for (int k = 0; k < pattern_count; k++) {
                        cJSON* patternObj = cJSON_GetArrayItem(patternArray, k);
                        MotorPattern* pattern = &dst->motorConfig->pattern[k];
                        
                        pattern->stepTime = cJSON_GetObjectItem(patternObj, "stepTime")->valueint;
                        pattern->direction = strdup(cJSON_GetObjectItem(patternObj, "direction")->valuestring);
                        pattern->pauseTime = cJSON_GetObjectItem(patternObj, "pauseTime")->valueint;
                    }
                }
                
                // Parse repeatTimes
                cJSON* repeatTimesObj = cJSON_GetObjectItem(motorConfigObj, "repeatTimes");
                dst->motorConfig->repeatTimes = repeatTimesObj ? repeatTimesObj->valueint : 1;
                
                // Parse runningStyle
                cJSON* runningStyleObj = cJSON_GetObjectItem(motorConfigObj, "runningStyle");
                dst->motorConfig->runningStyle = runningStyleObj ? 
                    strdup(runningStyleObj->valuestring) : strdup("Single Direction");
                    
                ESP_LOGI("CONFIG", "[MOTOR CONFIG] %s: %d patterns, repeat %u times, style: %s",
                         name, dst->motorConfig->pattern_count, 
                         (unsigned int)dst->motorConfig->repeatTimes,
                         dst->motorConfig->runningStyle);
            } else {
                dst->motorConfig = NULL;  // No motor configuration
            }

            // Legacy fields (keeping for backward compatibility)
            dst->stepTime = 0;
            dst->runningStyle = strdup("toggle");
            dst->pauseTime = 0;

            // Compute this component’s end time relative to phase start:
            uint32_t finish_time = dst->start + dst->duration;
            if (finish_time > max_phase_dur) {
                max_phase_dur = finish_time;
            }

            ESP_LOGI("CONFIG",
                     "[LOADED] %s (phase: %s)  start=%u  dur=%u",
                     name,
                     program_phases[i].name,
                     (unsigned int)dst->start,
                     (unsigned int)dst->duration);
        }

    }

    cJSON_Delete(root);
    free(content);
    return true;
}
void app_main(void) {
    // 7a) Mount SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    // Load JSON config
    if (!load_json_config("/spiffs/input.json")) {
        ESP_LOGE("APP", "Could not load configuration");
        return;
    }

    // Initialize all GPIO pins to OFF (1)
    for (int i = 0; i < NUM_COMPONENTS; i++) {
        gpio_reset_pin(component_states[i].pin);
        gpio_set_direction(component_states[i].pin, GPIO_MODE_OUTPUT);
        gpio_set_level(component_states[i].pin, 1);
    }

    // 7b) Run phases in order. We assume sorted by JSON order.
    
    // Start timer logger before first phase
    program_start_time = get_millis();
    timer_logger_running = true;
    
    ESP_LOGI("APP", "Starting program execution at t=%lu ms", (unsigned long)program_start_time);
    
    // Create timer logger task
    xTaskCreate(
        timer_logger_task,
        "timer_logger",
        2048,        // stack size
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL
    );
    
    uint32_t last_phase_start = 0;
    for (int i = 0; i < NUM_PHASES; i++) {
        Phase* p = &program_phases[i];

        // Compute how long to wait relative to previous phase start
        uint32_t this_delay = 0;
        if (p->startTime > last_phase_start) {
            this_delay = p->startTime - last_phase_start;
        }

        if (this_delay > 0) {
            vTaskDelay(pdMS_TO_TICKS(this_delay));
        }
        ESP_LOGI("APP", "Starting phase \"%s\" at t=%lu ms (program elapsed: %lu ms, delay=%lu)",
                 p->name, (unsigned long)get_millis(), (unsigned long)get_program_elapsed_ms(), (unsigned long)this_delay);

        // Run the phase (spawn component tasks & wait until all finish)
        run_phase(p);

        ESP_LOGI("APP", "Completed phase \"%s\" at t=%lu ms (program elapsed: %lu ms)",
                 p->name, (unsigned long)get_millis(), (unsigned long)get_program_elapsed_ms());

        // Update last_phase_start so the next phase’s delay is relative to this one
        last_phase_start = p->startTime;
    }

    // Stop timer logger
    timer_logger_running = false;
    vTaskDelay(pdMS_TO_TICKS(100)); // Give timer task time to exit

    ESP_LOGI("APP", "All phases complete at program elapsed: %lu ms. Entering idle.", (unsigned long)get_program_elapsed_ms());
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // keep the app alive
    }
}