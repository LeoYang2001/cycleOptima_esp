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
    const char* compId;
    gpio_num_t  pin;
    uint32_t    compStartTime;    // ms delay from phase start before running
    uint32_t    duration;         // how long (ms) to run this component
    uint32_t    stepTime;         // used for motor styles
    const char* runningStyle;     // "toggle" or "singleDir" (or future styles)
    uint32_t    pauseTime;        // only used if runningStyle == "singleDir"
    bool        isMotor;          // true if this component is any kind of motor
} ComponentInput;

typedef struct {
    const char*     phaseName;
    uint32_t        phaseStartTime;   // ms delay from “0” (or from last phase) to start this phase
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
    uint32_t    compStartTime;
    uint32_t    duration;
    uint32_t    stepTime;
    const char* runningStyle;
    uint32_t    pauseTime;
    bool        isMotor;
} ComponentTaskArg;



// --------------------------------------------------
// New function: all motor‐running logic goes here
// --------------------------------------------------
static void run_motor_task(const ComponentTaskArg* c) {
    if (strcmp(c->runningStyle, "toggle") == 0) {
        // ----- Toggle‐direction style -----
        uint32_t start = get_millis();
        bool direction_flag = false;

        // Initialize motor:
        gpio_set_level(MOTOR_DIRECTION_PIN, direction_flag);
        gpio_set_level(MOTOR_ON_PIN, 0);   // ON
        ESP_LOGI("COMPONENT_TASK",
                 "Motor (toggle) %s START at %lu ms for %lu ms (step=%lu ms)",
                 c->compId, (unsigned long)start, (unsigned long)c->duration, (unsigned long)c->stepTime);

        while (true) {
            uint32_t now = get_millis();
            if (now - start >= c->duration) {
                break;
            }

            // Wait stepTime before toggling
            vTaskDelay(pdMS_TO_TICKS(c->stepTime));

            // Pause motor for 1 s
            gpio_set_level(MOTOR_ON_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Flip direction and resume
            direction_flag = !direction_flag;
            gpio_set_level(MOTOR_DIRECTION_PIN, direction_flag);
            gpio_set_level(MOTOR_ON_PIN, 0);
            ESP_LOGI("COMPONENT_TASK",
                     "Motor (toggle) %s toggled direction to %d at %lu ms",
                     c->compId, direction_flag, (unsigned long)get_millis());
        }

        // End → turn OFF
        gpio_set_level(MOTOR_ON_PIN, 1);
        ESP_LOGI("COMPONENT_TASK", "Motor (toggle) %s FINISHED at %lu ms", c->compId, (unsigned long)get_millis());
    }
    else if (strcmp(c->runningStyle, "singleDir") == 0) {
        // ----- Single‐direction style -----
        uint32_t start = get_millis();

        // Fix direction once at start
        gpio_set_level(MOTOR_DIRECTION_PIN, true);
        gpio_set_level(MOTOR_ON_PIN, 0);    // ON
        ESP_LOGI("COMPONENT_TASK",
                 "Motor (singleDir) %s START at %lu ms for %lu ms (step=%lu ms, pause=%lu ms)",
                 c->compId, (unsigned long)start, (unsigned long)c->duration, (unsigned long)c->stepTime, (unsigned long)c->pauseTime);

        while (true) {
            uint32_t now     = get_millis();
            uint32_t elapsed = now - start;
            if (elapsed >= c->duration) {
                break;
            }

            // Compute ON/OFF cycle
            uint32_t cycle_len = c->stepTime + c->pauseTime;
            uint32_t rel       = elapsed % cycle_len;
            if (rel < c->stepTime) {
                gpio_set_level(MOTOR_ON_PIN, 0);   // ON
            } else {
                gpio_set_level(MOTOR_ON_PIN, 1);   // Pause
            }

            // Short sleep to avoid busy‐looping
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // End → turn OFF
        gpio_set_level(MOTOR_ON_PIN, 1);
        ESP_LOGI("COMPONENT_TASK", "Motor (singleDir) %s FINISHED at %lu ms", c->compId, (unsigned long)get_millis());
    }
    else {
        // ----- Unknown style -----
        ESP_LOGW("COMPONENT_TASK",
                 "Motor %s has unknown runningStyle \"%s\"",
                 c->compId, c->runningStyle);
    }
}
static void component_task(void* arg) {
    ComponentTaskArg* c = (ComponentTaskArg*)arg;

    // 1) Wait until it’s time to start
    if (c->compStartTime > 0) {
        vTaskDelay(pdMS_TO_TICKS(c->compStartTime));
    }

    if (c->isMotor) {
        // Delegate all motor logic to run_motor_task()
        run_motor_task(c);
    }
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
        uint32_t finish_time = comp->compStartTime + comp->duration;
        if (finish_time > phase_duration) {
            phase_duration = finish_time;
        }

        // Build argument struct for the component task
        ComponentTaskArg* arg = malloc(sizeof(ComponentTaskArg));
        *arg = (ComponentTaskArg){
            .compId         = comp->compId,
            .pin            = comp->pin,
            .compStartTime  = comp->compStartTime,
            .duration       = comp->duration,
            .stepTime       = comp->stepTime,
            .runningStyle   = comp->runningStyle,
            .pauseTime      = comp->pauseTime,
            .isMotor        = comp->isMotor
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
        program_phases[i].phaseName = strdup(cJSON_GetObjectItem(phaseObj, "phaseName")->valuestring);
        program_phases[i].phaseStartTime = cJSON_GetObjectItem(phaseObj, "phaseStartTime")->valueint;

        cJSON* comps = cJSON_GetObjectItem(phaseObj, "components");
        int num_comps = cJSON_GetArraySize(comps);
        program_phases[i].components = calloc(num_comps, sizeof(ComponentInput));
        program_phases[i].num_components = num_comps;

        // Track the maximum (compStartTime + duration) so we know phase duration:
        uint32_t max_phase_dur = 0;

        for (int j = 0; j < num_comps; j++) {
            cJSON* compObj = cJSON_GetArrayItem(comps, j);
            ComponentInput* dst = &program_phases[i].components[j];

            const char* name = cJSON_GetObjectItem(compObj, "compId")->valuestring;
            dst->compId = strdup(name);
            dst->pin    = map_name_to_pin(name);
            dst->compStartTime = cJSON_GetObjectItem(compObj, "compStartTime")->valueint;
            dst->duration      = cJSON_GetObjectItem(compObj, "duration")->valueint;

            cJSON* stepObj = cJSON_GetObjectItem(compObj, "stepTime");
            dst->stepTime = stepObj ? stepObj->valueint : 0;

            cJSON* ifMotor = cJSON_GetObjectItem(compObj, "ifMotor");
            dst->isMotor = cJSON_IsTrue(ifMotor);

            // New fields:
            cJSON* styleObj = cJSON_GetObjectItem(compObj, "runningStyle");
            if (styleObj && styleObj->valuestring) {
                dst->runningStyle = strdup(styleObj->valuestring);
            } else {
                dst->runningStyle = strdup("toggle"); // default
            }

            cJSON* pauseObj = cJSON_GetObjectItem(compObj, "pauseTime");
            dst->pauseTime = pauseObj ? pauseObj->valueint : 0;

            // Compute this component’s end time relative to phase start:
            uint32_t finish_time = dst->compStartTime + dst->duration;
            if (finish_time > max_phase_dur) {
                max_phase_dur = finish_time;
            }

            ESP_LOGI("CONFIG",
                     "[LOADED] %s (phase: %s)  start=%lu  dur=%lu  motor=%d  style=%s  pause=%lu",
                     name,
                     program_phases[i].phaseName,
                     dst->compStartTime,
                     dst->duration,
                     dst->isMotor,
                     dst->runningStyle,
                     dst->pauseTime);
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
        if (p->phaseStartTime > last_phase_start) {
            this_delay = p->phaseStartTime - last_phase_start;
        }

        if (this_delay > 0) {
            vTaskDelay(pdMS_TO_TICKS(this_delay));
        }
        ESP_LOGI("APP", "Starting phase \"%s\" at t=%lu ms (delay=%lu)",
                 p->phaseName, get_millis(), this_delay);

        // Run the phase (spawn component tasks & wait until all finish)
        run_phase(p);

        ESP_LOGI("APP", "Completed phase \"%s\" at t=%lu ms",
                 p->phaseName, get_millis());

        // Update last_phase_start so the next phase’s delay is relative to this one
        last_phase_start = p->phaseStartTime;
    }

    ESP_LOGI("APP", "All phases complete. Entering idle.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // keep the app alive
    }
}