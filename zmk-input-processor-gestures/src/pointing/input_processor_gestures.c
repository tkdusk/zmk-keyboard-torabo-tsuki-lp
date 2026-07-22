/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Gesture detection with k_work_delayable for safe behavior invocation.
 * Behavior is fired from the system work queue (not the input processor
 * context) to avoid deadlocks with BLE/ZMK Studio in cormoran fork.
 */

#define DT_DRV_COMPAT zmk_input_processor_gestures

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

enum gesture_direction {
    GESTURE_UP    = 0,
    GESTURE_DOWN  = 1,
    GESTURE_LEFT  = 2,
    GESTURE_RIGHT = 3,
    GESTURE_DIRECTION_COUNT,
};

struct gestures_config {
    uint8_t index;
    uint8_t type;
    uint16_t x_code;
    uint16_t y_code;
    uint16_t threshold;
    uint16_t timeout_ms;
    uint16_t cooldown_ms;
    uint16_t tap_ms;
    const struct zmk_behavior_binding *bindings;
    uint32_t active_layers;
};

struct gestures_data {
    int32_t x;
    int32_t y;
    int64_t started_at;
    int64_t cooldown_until;
    bool pending;

    /* Work items for deferred press/release (avoids deadlock in input context) */
    const struct device *dev;
    uint8_t device_index;
    int fired_direction;
    struct k_work_delayable press_work;
    struct k_work_delayable release_work;
};

static int32_t abs32(int32_t value) { return value < 0 ? -value : value; }

static void gestures_reset(struct gestures_data *data) {
    data->x = 0;
    data->y = 0;
    data->started_at = 0;
    data->pending = false;
}

static int gestures_direction(const struct gestures_config *cfg,
                              const struct gestures_data *data) {
    int32_t abs_x = abs32(data->x);
    int32_t abs_y = abs32(data->y);

    if (abs_x < cfg->threshold && abs_y < cfg->threshold) {
        return -ENODATA;
    }

    if (abs_y >= abs_x) {
        return data->y < 0 ? GESTURE_UP : GESTURE_DOWN;
    }

    return data->x < 0 ? GESTURE_LEFT : GESTURE_RIGHT;
}

static struct zmk_behavior_binding_event make_event(const struct gestures_data *data,
                                                     const struct gestures_config *cfg) {
    struct zmk_behavior_binding_event ev = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(
            data->device_index, cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    return ev;
}

static void release_work_cb(struct k_work *work) {
    struct k_work_delayable *d = k_work_delayable_from_work(work);
    struct gestures_data *data = CONTAINER_OF(d, struct gestures_data, release_work);
    const struct gestures_config *cfg = data->dev->config;

    if (data->fired_direction < 0 || data->fired_direction >= GESTURE_DIRECTION_COUNT) {
        return;
    }

    const struct zmk_behavior_binding *binding = &cfg->bindings[data->fired_direction];
    struct zmk_behavior_binding_event ev = make_event(data, cfg);

    int ret = zmk_behavior_invoke_binding(binding, ev, false);
    if (ret < 0) {
        LOG_WRN("gesture: release failed (%d)", ret);
    }

    data->fired_direction = -1;
}

static void press_work_cb(struct k_work *work) {
    struct k_work_delayable *d = k_work_delayable_from_work(work);
    struct gestures_data *data = CONTAINER_OF(d, struct gestures_data, press_work);
    const struct gestures_config *cfg = data->dev->config;

    if (data->fired_direction < 0 || data->fired_direction >= GESTURE_DIRECTION_COUNT) {
        return;
    }

    const struct zmk_behavior_binding *binding = &cfg->bindings[data->fired_direction];
    struct zmk_behavior_binding_event ev = make_event(data, cfg);

    int ret = zmk_behavior_invoke_binding(binding, ev, true);
    if (ret < 0) {
        LOG_WRN("gesture: press failed (%d), skipping release", ret);
        data->fired_direction = -1;
        return;
    }

    /* Schedule release after tap_ms; fired_direction cleared in release_work_cb */
    k_work_schedule(&data->release_work, K_MSEC(cfg->tap_ms));
}

static int gestures_handle_event(const struct device *dev, struct input_event *event,
                                 uint32_t param1, uint32_t param2,
                                 struct zmk_input_processor_state *state) {
    const struct gestures_config *cfg = dev->config;
    struct gestures_data *data = dev->data;

    if (event->type != cfg->type ||
        (event->code != cfg->x_code && event->code != cfg->y_code)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Layer filter: pass through when no matching layer active */
    if (cfg->active_layers != 0) {
        bool layer_active = false;
        for (int i = 0; i < 32; i++) {
            if ((cfg->active_layers & BIT(i)) && zmk_keymap_layer_active(i)) {
                layer_active = true;
                break;
            }
        }
        if (!layer_active) {
            gestures_reset(data);
            return ZMK_INPUT_PROC_CONTINUE;
        }
    }

    int32_t delta = event->value;
    int64_t now = k_uptime_get();
    event->value = 0;

    if (now < data->cooldown_until) {
        return ZMK_INPUT_PROC_STOP;
    }

    if (data->pending && now - data->started_at > cfg->timeout_ms) {
        gestures_reset(data);
    }

    if (!data->pending) {
        if (delta == 0) {
            return ZMK_INPUT_PROC_STOP;
        }
        data->started_at = now;
        data->pending = true;
    }

    if (event->code == cfg->x_code) {
        data->x += delta;
    } else {
        data->y += delta;
    }

    if (!event->sync) {
        return ZMK_INPUT_PROC_STOP;
    }

    int direction = gestures_direction(cfg, data);
    if (direction >= 0) {
        /* Store context and defer behavior invocation to system work queue */
        data->device_index = state->input_device_index;
        data->fired_direction = direction;
        k_work_schedule(&data->press_work, K_NO_WAIT);

        gestures_reset(data);
        data->cooldown_until = now + cfg->cooldown_ms;
    }

    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api gestures_driver_api = {
    .handle_event = gestures_handle_event,
};

static int gestures_init(const struct device *dev) {
    struct gestures_data *data = dev->data;
    data->dev = dev;
    data->fired_direction = -1;
    k_work_init_delayable(&data->press_work, press_work_cb);
    k_work_init_delayable(&data->release_work, release_work_cb);
    return 0;
}

#define TRANSFORMED_BINDINGS(n)                                                                \
    {LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}

#define GESTURES_INST(n)                                                                       \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == GESTURE_DIRECTION_COUNT,                    \
                 "Gesture processor needs exactly 4 bindings: up, down, left, right");        \
    static struct gestures_data gestures_data_##n = {};                                        \
    static struct zmk_behavior_binding gestures_bindings_##n[] = TRANSFORMED_BINDINGS(n);     \
    static const struct gestures_config gestures_config_##n = {                                \
        .index = n,                                                                            \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                       \
        .x_code = DT_INST_PROP_OR(n, x_code, INPUT_REL_X),                                    \
        .y_code = DT_INST_PROP_OR(n, y_code, INPUT_REL_Y),                                    \
        .threshold = DT_INST_PROP_OR(n, threshold, 120),                                      \
        .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 180),                                    \
        .cooldown_ms = DT_INST_PROP_OR(n, cooldown_ms, 220),                                  \
        .tap_ms = DT_INST_PROP_OR(n, tap_ms, 30),                                             \
        .bindings = gestures_bindings_##n,                                                     \
        .active_layers = DT_INST_PROP_OR(n, active_layers, 0),                                \
    };                                                                                         \
    DEVICE_DT_INST_DEFINE(n, gestures_init, NULL, &gestures_data_##n,                         \
                          &gestures_config_##n, POST_KERNEL,                                   \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gestures_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GESTURES_INST)
