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
    {"Detergent Valve", DETERGENT_VALVE_PIN, 0, false},
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
    const char* compId;
    gpio_num_t  pin;
    uint32_t    start;    // ms delay from phase start before running
    uint32_t    duration;         // how long (ms) to run this component
    uint32_t    stepTime;         // used for motor styles
    const char* runningStyle;     // "toggle" or "singleDir" (or future styles)
    uint32_t    pauseTime;        // only used if runningStyle == "singleDir"
} ComponentInput;

typedef struct {
    const char*     name;
    uint32_t        startTime;   // ms delay from “0” (or from last phase) to start this phase
    ComponentInput* components;
    int             num_components;
} Phase;

Phase* program_phases = NULL;
int NUM_PHASES = 0;

uint32_t get_millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

typedef struct {
    const char* compId;
    gpio_num_t  pin;
    uint32_t    start;
    uint32_t    duration;
} ComponentTaskArg;



// --------------------------------------------------
// New function: all motor‐running logic goes here
// --------------------------------------------------
static void run_motor_task(const ComponentTaskArg* c) {
    ESP_LOGI("COMPONENT_TASK", "Running motor task for %s", c->compId);

    // Motor control logic goes here
}
static void component_task(void* arg) {
    ComponentTaskArg* c = (ComponentTaskArg*)arg;

    // 1) Wait until it’s time to start
    if (c->start > 0) {
        vTaskDelay(pdMS_TO_TICKS(c->start));
    }

    // if (c->isMotor) {
    //     // Delegate all motor logic to run_motor_task()
    //     run_motor_task(c);
    // }
    else {
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
        };

        // Name each task after the component for easier debugging:
        char task_name[MAX_TASK_NAME_LEN];
        snprintf(task_name, MAX_TASK_NAME_LEN, "comp_%s", comp->compId);

        // Create the FreeRTOS task:
        xTaskCreate(
            component_task,
            task_name,
            4096,        // stack size; 
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
        ESP_LOGI("APP", "Starting phase \"%s\" at t=%lu ms (delay=%lu)",
                 p->name, get_millis(), this_delay);

        // Run the phase (spawn component tasks & wait until all finish)
        run_phase(p);

        ESP_LOGI("APP", "Completed phase \"%s\" at t=%lu ms",
                 p->name, get_millis());

        // Update last_phase_start so the next phase’s delay is relative to this one
        last_phase_start = p->startTime;
    }

    ESP_LOGI("APP", "All phases complete. Entering idle.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // keep the app alive
    }
}