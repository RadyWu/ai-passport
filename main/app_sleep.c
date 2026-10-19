#include "app_sleep.h"
#include "app_net.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_sleep";

static void warn(const char *step, esp_err_t e)
{
    if (e != ESP_OK) ESP_LOGW(TAG, "%s: %s", step, esp_err_to_name(e));
}

/** Sustained ADC release — double-click OK often still low when sleep is queued. */
static bool wait_keys_released_adc(int timeout_ms)
{
    const int sample_ms = 40;
    const int need_ok = 10; /* ~400 ms continuous release */
    int ok = 0;
    int waited = 0;
    while (waited < timeout_ms) {
        int mv = bsp_button_read_mv();
        if (mv >= BSP_BTN_MV_RELEASED_MIN) {
            if (++ok >= need_ok) {
                ESP_LOGI(TAG, "keys released (adc=%d mV)", mv);
                return true;
            }
        } else {
            ok = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(sample_ms));
        waited += sample_ms;
    }
    ESP_LOGW(TAG, "keys still held after %d ms (adc=%d)",
             timeout_ms, bsp_button_read_mv());
    return false;
}

static bool wait_gpio_high(gpio_num_t pin, int timeout_ms)
{
    const int sample_ms = 20;
    const int need_ok = 15; /* ~300 ms continuous high */
    int ok = 0;
    int waited = 0;
    while (waited < timeout_ms) {
        if (gpio_get_level(pin) == 1) {
            if (++ok >= need_ok) {
                ESP_LOGI(TAG, "GPIO%d stable HIGH (%d ms)", (int)pin, ok * sample_ms);
                return true;
            }
        } else {
            ok = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(sample_ms));
        waited += sample_ms;
    }
    ESP_LOGW(TAG, "GPIO%d still low after %d ms", (int)pin, timeout_ms);
    return false;
}

bool app_sleep_enter_deep(void)
{
    /*
     * Abort paths below must NOT esp_restart() — that looked like "self-wake".
     * Only terminal failure after radio/display teardown uses restart.
     */
    if (!wait_keys_released_adc(3000)) {
        ESP_LOGW(TAG, "abort sleep: key held (ADC)");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    warn("clear wakes", esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));

    int level = 0;
    esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button prepare failed (%s) — abort sleep", esp_err_to_name(err));
        return false;
    }

    const gpio_num_t pin = (gpio_num_t)BSP_BTN_GPIO;
    if (level != 1 && !wait_gpio_high(pin, 2500)) {
        ESP_LOGW(TAG, "abort sleep: pad still LOW after hand-off (would self-wake)");
        return false;
    }
    if (!wait_gpio_high(pin, 500)) {
        /* Even if first sample was 1, demand a short stable stretch. */
        ESP_LOGW(TAG, "abort sleep: pad not stable HIGH");
        return false;
    }

    warn("gpio wake", esp_deep_sleep_enable_gpio_wakeup(
             1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));

    warn("battery", bsp_battery_sleep());
    warn("audio", bsp_audio_sleep());
    warn("i2s", bsp_audio_prepare_deep_sleep());
    warn("i2c", bsp_i2c_prepare_deep_sleep());

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock failed — abort sleep (radio still up)");
        return false;
    }
    warn("display", bsp_display_prepare_deep_sleep());
    bsp_lvgl_unlock();

    if (gpio_get_level(pin) != 1) {
        ESP_LOGW(TAG, "GPIO%d low before radio stop — abort sleep", (int)pin);
        return false;
    }

    /* Stop SoftAP/Wi-Fi only when we are committed to sleep. */
    app_net_stop_for_sleep();

    if (gpio_get_level(pin) != 1) {
        /* Radio already down — must reboot to recover networking. */
        ESP_LOGE(TAG, "GPIO%d low after radio stop — restart to recover", (int)pin);
        esp_restart();
    }

    ESP_LOGI(TAG, "entering deep sleep (wake=GPIO%d LOW only)", (int)pin);
    esp_deep_sleep_start();
    esp_restart();
    return false;
}
