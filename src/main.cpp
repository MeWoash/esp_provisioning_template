#include <esp_log.h>
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "MAIN";

#ifdef __cplusplus
extern "C"
{
#endif

    void app_main()
    {

        // Start Wi-Fi task
        ESP_LOGI(TAG, "Starting Wi-Fi monitoring task...");
        xTaskCreate(wifi_manager_task, "wifi_manager_task", 4096, NULL, 5, NULL);
        wifi_manager_trigger_init();
        wifi_manager_trigger_connect();

        vTaskDelay(pdMS_TO_TICKS(10000));

        wifi_manager_trigger_disconnect();
        vTaskDelay(pdMS_TO_TICKS(10000));
        wifi_manager_trigger_deinit();

        vTaskDelay(pdMS_TO_TICKS(10000));

        wifi_manager_trigger_init();
        vTaskDelay(pdMS_TO_TICKS(10000));
        wifi_manager_trigger_connect();

        while (true)
        {
            vTaskDelay(portMAX_DELAY);
        }
    }

#ifdef __cplusplus
}
#endif