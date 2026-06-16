#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <math.h>

#define NUMLOCK_BIT BIT(0)
#define CAPSLOCK_BIT BIT(1)
#define SCROLLLOCK_BIT BIT(2)

#define COLOR_RED BIT(0)
#define COLOR_GREEN BIT(1)
#define COLOR_BLUE BIT(2)
#define COLOR_WHITE (COLOR_RED | COLOR_GREEN | COLOR_BLUE)
#define COLOR_YELLOW (COLOR_RED | COLOR_GREEN)
#define COLOR_MAGENTA (COLOR_RED | COLOR_BLUE)
#define COLOR_OFF 0

#define BATTERY_LOW_PERCENT 10
#define BT_PROFILE_COUNT 4

// Per-profile connection color: BT0 blue, BT1 yellow, BT2 magenta.
static const uint8_t profile_color[3] = {COLOR_BLUE, COLOR_YELLOW, COLOR_MAGENTA};

static uint8_t active_profile_color(void) {
    uint8_t idx = zmk_ble_active_profile_index();
    if (idx < ARRAY_SIZE(profile_color)) {
        return profile_color[idx];
    }
    return COLOR_BLUE;
}
#define FEEDBACK_TICKS 150  // ~3s of connection-change feedback at 20ms/tick

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_r)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_g)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_b)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(indicator_r)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(indicator_g)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(indicator_b))};

struct indicator_state_t {
    uint8_t keylock;
    uint8_t battery;
    uint16_t feedback_ticks;  // remaining ticks to show solid connection color
} indicator_state;

static void set_indicator_color(uint8_t bits) {
    static uint8_t last_bits = 0;
    if (bits != last_bits) {
        for (uint8_t pos = 0; pos < 3; pos++) {
            if (bits & (1<<pos)) {
                led_on(led_dev, led_idx[pos]);
            } else {
                led_off(led_dev, led_idx[pos]);
            }
        }
        last_bits = bits;
    }
}

static void get_lock_indicators(void) {
    uint8_t state = zmk_hid_indicators_get_current_profile();
    LOG_DBG("LOCK LEDS: %d", state);
    indicator_state.keylock = state;
}

static void hid_indicators_status_update_cb(const zmk_event_t *eh) {
    get_lock_indicators();
}

ZMK_LISTENER(widget_hid_indicators_status, hid_indicators_status_update_cb);
ZMK_SUBSCRIPTION(widget_hid_indicators_status, zmk_hid_indicators_changed);


struct blink_item {
    uint16_t duration_ms;
    uint16_t sleep_ms;
    uint8_t count;
};

K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);


// Show the solid connection color (USB white / BLE blue) for a few seconds
// after any connection change, then fall back to the Caps Lock indicator.
static void show_connection_feedback(void) {
    indicator_state.feedback_ticks = FEEDBACK_TICKS;
}

static void ble_active_profile_update(void) {
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (profile_index >= BT_PROFILE_COUNT) return;
    show_connection_feedback();
    LOG_DBG("Device_BT%d feedback", profile_index + 1);
}

static void ble_active_profile_update_cb(const zmk_event_t *eh) {
    ble_active_profile_update();
}

ZMK_LISTENER(ble_active_profile_listener, ble_active_profile_update_cb);
ZMK_SUBSCRIPTION(ble_active_profile_listener, zmk_ble_active_profile_changed);

// USB <-> BLE transport switches don't emit SHOW_LED, so trigger feedback here too.
static int endpoint_changed_cb(const zmk_event_t *eh) {
    show_connection_feedback();
    return 0;
}

ZMK_LISTENER(led_endpoint_listener, endpoint_changed_cb);
ZMK_SUBSCRIPTION(led_endpoint_listener, zmk_endpoint_changed);

#include <zmk/events/keycode_state_changed.h>
static int zmk_handle_keycode_user(struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;
    LOG_DBG("key 0x%X", key);
    if (key == 0xAB) {
        ble_active_profile_update();
    }
    return ZMK_EV_EVENT_HANDLED;
}

static int keycode_user_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc_state;

    kc_state = as_zmk_keycode_state_changed(eh);

    if (kc_state != NULL) {
        return zmk_handle_keycode_user(kc_state);
    }

    return 0;
}

ZMK_LISTENER(keycode_user, keycode_user_listener);
ZMK_SUBSCRIPTION(keycode_user, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    indicator_state.battery = battery_level;
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

void led_process_thread(void) {
    while (true) {
        k_sleep(K_MSEC(20));

        static uint16_t led_timer_steps = 0;
        led_timer_steps++;

        // Priority 1: low battery is always solid red.
        if (indicator_state.battery < BATTERY_LOW_PERCENT) {
            set_indicator_color(COLOR_RED);
            continue;
        }

        bool on_usb = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB);

        // Priority 2: BLE pairing in progress (profile open, not yet connected)
        // blinks the profile color. ZMK reports USB as the selected output when
        // a cable is attached but BLE hasn't connected yet, so don't let that
        // white mask an active pairing attempt: on battery blink the whole time;
        // on USB fallback blink while a recent BT key press is still in its
        // feedback window. A USB-only user who never pressed a BT key keeps the
        // white indicator.
        if (zmk_ble_active_profile_is_open() &&
            !zmk_ble_active_profile_is_connected() &&
            (!on_usb || indicator_state.feedback_ticks > 0)) {
            if (indicator_state.feedback_ticks > 0) {
                indicator_state.feedback_ticks--;
            }
            if ((led_timer_steps >> 3) & 0x1) {
                set_indicator_color(active_profile_color());
            } else {
                set_indicator_color(COLOR_OFF);
            }
            continue;
        }

        // Priority 3: for a few seconds after a connection change, show the
        // solid connection color: white on USB, the per-profile color on a
        // connected BLE profile (BT0 blue / BT1 yellow / BT2 magenta).
        if (indicator_state.feedback_ticks > 0) {
            indicator_state.feedback_ticks--;
            if (on_usb) {
                set_indicator_color(COLOR_WHITE);
            } else if (zmk_ble_active_profile_is_connected()) {
                set_indicator_color(active_profile_color());
            } else {
                set_indicator_color(COLOR_OFF);
            }
            continue;
        }

        // Priority 4: Caps Lock is solid green, otherwise the LED is off.
        if (indicator_state.keylock & CAPSLOCK_BIT) {
            set_indicator_color(COLOR_GREEN);
        } else {
            set_indicator_color(COLOR_OFF);
        }
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 100);


void klink_indicator_init_thread(void) {
    indicator_state.feedback_ticks = 0;
    // zmk_ble_set_device_name("Tofu60 v3.0z BLE");
    indicator_state.battery = 111;
}

K_THREAD_DEFINE(klink_indicator_init_tid, 1024, klink_indicator_init_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 200);