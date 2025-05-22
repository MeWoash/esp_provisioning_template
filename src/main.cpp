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

        while (wifi_manager_get_connection_state() != Wifi_conn_state::connected)
        {
            // ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "****************** WIFI CONNECTED ******************\n\n");

        wifi_manager_trigger_disconnect();
        while (wifi_manager_get_connection_state() != Wifi_conn_state::disconnected)
        {
            // ESP_LOGI(TAG, "Waiting for Wi-Fi disconnection...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "****************** WIFI DISCONNECTED ******************\n\n");

        wifi_manager_trigger_deinit();
        while (wifi_manager_get_connection_state() != Wifi_conn_state::not_initialized)
        {
            // ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "****************** WIFI DEINITIALIZED ******************\n\n");

        wifi_manager_trigger_init();
        while (wifi_manager_get_connection_state() != Wifi_conn_state::disconnected)
        {
            // ESP_LOGI(TAG, "Waiting for Wi-Fi init");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "****************** WIFI INITIALIZED ******************\n\n");

        wifi_manager_trigger_connect();
        while (wifi_manager_get_connection_state() != Wifi_conn_state::connected)
        {
            // ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "****************** WIFI CONNECTED ******************\n\n");

        while (true)
        {
            ESP_LOGI(TAG, "Wi-Fi connected. Running main loop...");
            vTaskDelay(portMAX_DELAY);
        }
    }

#ifdef __cplusplus
}
#endif