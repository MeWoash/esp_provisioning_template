#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_event.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

// TYPES

enum Wifi_Events
{
    WIFI_EVENT_START_INITIALIZATION = 1 << 0,
    WIFI_EVENT_START_CONNECTING = 1 << 1,
    WIFI_EVENT_CONNECTION_FAILED = 1 << 2,
    WIFI_EVENT_CONNECTION_SUCCESS = 1 << 3,
    WIFI_EVENT_START_DISCONNECTING = 1 << 4,
    WIFI_EVENT_START_DEINIT = 1 << 5,
    WIFI_EVENT_BLE_PROV_ENDED = 1 << 6,
};

// CONSTATNTS
static constexpr auto TAG = "WIFI_MGR";
static constexpr auto TAG_HANDLER = "WIFI_EVENT_HANDLER";
static constexpr auto *POP = "abcd1234"; // Proof-of-possession

// GLOBALS
static Wifi_conn_state con_state = Wifi_conn_state::not_initialized;
static Wifi_con_method con_method = Wifi_con_method::none;
static char service_name[12] = "PROV_XXXX";
static TaskHandle_t wifi_init_task_handle = nullptr;
static uint8_t connection_attempts = 0;

EventGroupHandle_t wifi_events = xEventGroupCreate();

// STATIC DECLARATIONS
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data);

static void wifi_run_ble();
static void wifi_stop_ble();
static void wifi_init_worker(void *params);
static void wifi_deinit_worker(void *params);
static void wifi_run_ble_prov_worker(void *params);
static void wifi_stop_ble_prov_worker(void *params);

// INTERFACE
void wifi_manager_trigger_init()
{
    xEventGroupSetBits(wifi_events, WIFI_EVENT_START_INITIALIZATION);
}

void wifi_manager_trigger_deinit()
{
    xEventGroupSetBits(wifi_events, WIFI_EVENT_START_DEINIT);
}

void wifi_manager_trigger_connect()
{
    xEventGroupSetBits(wifi_events, WIFI_EVENT_START_CONNECTING);
}

void wifi_manager_trigger_disconnect()
{
    xEventGroupSetBits(wifi_events, WIFI_EVENT_START_DISCONNECTING);
}

void wifi_manager_task(void *params)
{
    wifi_init_task_handle = xTaskGetCurrentTaskHandle();
    while (1)
    {
        ESP_LOGI(TAG, "Wi-Fi manager state: %d", static_cast<int>(con_state));
        switch (con_state)
        {
        case Wifi_conn_state::not_initialized:
        {
            ESP_LOGI(TAG, "Wi-Fi manager state: not initialized");
            xEventGroupWaitBits(
                wifi_events,
                WIFI_EVENT_START_INITIALIZATION,
                pdTRUE,       // clear on exit
                pdFALSE,      // wait for any bit
                portMAX_DELAY // block forever
            );
            xTaskCreate(wifi_init_worker, "wifi_init_worker", 4096, wifi_init_task_handle, 5, NULL);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            con_state = Wifi_conn_state::disconnected;
            break;
        }

        case Wifi_conn_state::disconnected:
        {
            ESP_LOGI(TAG, "Wi-Fi manager state: disconnected");
            EventBits_t event_bits = xEventGroupWaitBits(
                wifi_events,
                WIFI_EVENT_START_CONNECTING | WIFI_EVENT_START_DEINIT,
                pdTRUE,       // clear on exit
                pdFALSE,      // wait for any bit
                portMAX_DELAY // block forever
            );
            if (event_bits & WIFI_EVENT_START_DEINIT)
            {
                xTaskCreate(wifi_deinit_worker, "wifi_deinit_worker", 4096, wifi_init_task_handle, 5, NULL);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                con_state = Wifi_conn_state::not_initialized;
            }
            else if (event_bits & WIFI_EVENT_START_CONNECTING)
            {
                con_state = Wifi_conn_state::connecting;
            }
            break;
        }
        case Wifi_conn_state::connecting:
        {
            ESP_LOGI(TAG, "Wi-Fi manager state: connecting");
            switch (con_method)
            {
            case Wifi_con_method::none:
            {
                ESP_LOGI(TAG, "Wi-Fi connection method: none");
                bool provisioned = false;
                wifi_prov_mgr_is_provisioned(&provisioned);

                if (provisioned)
                {
                    ESP_LOGI(TAG, "Wi-Fi connection method switch: saved credentials");
                    con_method = Wifi_con_method::saved_credentials;
                }
                else
                {
                    ESP_LOGI(TAG, "Wi-Fi connection method switch: ble provisioning");
                    con_method = Wifi_con_method::ble_provisioning;
                }
                break;
            }
            case Wifi_con_method::saved_credentials:
            {
                ESP_LOGI(TAG, "Wi-Fi manager state: connecting with saved credentials");
                auto err = esp_wifi_connect();

                EventBits_t event_bits = xEventGroupWaitBits(
                    wifi_events,
                    WIFI_EVENT_CONNECTION_FAILED | WIFI_EVENT_CONNECTION_SUCCESS,
                    pdTRUE,              // clear on exit
                    pdFALSE,             // wait for any bit
                    pdMS_TO_TICKS(10000) // block 10s
                );
                if (event_bits & WIFI_EVENT_CONNECTION_SUCCESS)
                {
                    con_state = Wifi_conn_state::connected;
                    con_method = Wifi_con_method::none;
                }
                else
                {
                    ESP_LOGE(TAG, "Wi-Fi connection attempt %d/%d failed: %s", connection_attempts, CONFIG_WIFI_CONN_RETRY_MAX, esp_err_to_name(err));
                    if (connection_attempts < CONFIG_WIFI_CONN_RETRY_MAX)
                    {
                        connection_attempts++;
                    }
                    else
                    {
                        connection_attempts = 0;
                        // TRY AGAIN
                        con_method = Wifi_con_method::ble_provisioning; // Switch to BLE provisioning
                        ESP_LOGI(TAG, "Wi-Fi connection method switch: ble provisioning");
                    }
                }
                break;
            }
            case Wifi_con_method::ble_provisioning:
            {
                ESP_LOGI(TAG, "Wi-Fi manager state: connecting with BLE provisioning");
                xTaskCreate(wifi_run_ble_prov_worker, "wifi_ble_prov_worker", 4096, wifi_init_task_handle, 5, NULL);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                EventBits_t event_bits = xEventGroupWaitBits(
                    wifi_events,
                    WIFI_EVENT_CONNECTION_FAILED | WIFI_EVENT_CONNECTION_SUCCESS,
                    pdTRUE,  // clear on exit
                    pdFALSE, // wait for any bit
                    pdMS_TO_TICKS(1000 * 60 * 5));
                if (event_bits & WIFI_EVENT_CONNECTION_SUCCESS)
                {
                    ESP_LOGI(TAG, "Connection successful with BLE. Waiting to safely stop BLE provisioning and save credentials");
                    xEventGroupWaitBits(
                        wifi_events,
                        WIFI_EVENT_CONNECTION_FAILED | WIFI_EVENT_CONNECTION_SUCCESS,
                        pdTRUE,         // clear on exit
                        pdFALSE,        // wait for any bit
                        portMAX_DELAY); // block forever
                    ESP_LOGI(TAG, "Running stop BLE provisioning");
                    xTaskCreate(wifi_stop_ble_prov_worker, "wifi_stop_ble_prov_worker", 4096, wifi_init_task_handle, 5, NULL);
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    con_state = Wifi_conn_state::connected;
                    con_method = Wifi_con_method::none;
                }
                else
                {
                    ESP_LOGW(TAG, "BLE provisioning failed, restarting BLE provisioning");
                    xTaskCreate(wifi_stop_ble_prov_worker, "wifi_stop_ble_prov_worker", 4096, wifi_init_task_handle, 5, NULL);
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                }
                break;
            }
            }

            break;
        }
        case Wifi_conn_state::connected:
        {
            ESP_LOGI(TAG, "Wi-Fi manager state: connected");
            xEventGroupWaitBits(
                wifi_events,
                WIFI_EVENT_START_DISCONNECTING,
                pdTRUE,       // clear on exit
                pdFALSE,      // wait for any bit
                portMAX_DELAY // block forever
            );
            esp_wifi_disconnect();
            con_state = Wifi_conn_state::disconnected;
            break;
        }
        default:
            ESP_LOGI(TAG, "Wi-Fi manager state: unknown(%d)", static_cast<int>(con_state));
            break;
        }
    }
    vTaskDelete(NULL);
}

// STATICS

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{

    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_CONNECTED:
        {
            wifi_event_sta_connected_t *connected = (wifi_event_sta_connected_t *)event_data;
            ESP_LOGI(TAG_HANDLER, "WIFI_EVENT_STA_CONNECTED\t SSID: %s", (const char *)connected->ssid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGI(TAG_HANDLER, "WIFI_EVENT_STA_DISCONNECTED\t SSID: %s, Reason: %d", disconnected->ssid, static_cast<int>(disconnected->reason));
            xEventGroupSetBits(wifi_events, WIFI_EVENT_CONNECTION_FAILED);
            break;
        }
        default:
            ESP_LOGI(TAG_HANDLER, "Unhandled Wi-Fi event ID: %d", static_cast<int>(event_id));
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG_HANDLER, "IP_EVENT_STA_GOT_IP\t IP: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(wifi_events, WIFI_EVENT_CONNECTION_SUCCESS);
            break;
        }

        default:
            break;
        }
    }
    else if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_INIT:
            ESP_LOGI(TAG_HANDLER, "WIFI_PROV_INIT");
            break;

        case WIFI_PROV_START:
            ESP_LOGI(TAG_HANDLER, "WIFI_PROV_START");
            break;

        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG_HANDLER, "WIFI_PROV_CRED_RECV"
                                  "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }

        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG_HANDLER, "WIFI_PROV_CRED_FAIL\n\tReason : %s"
                                  "\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
            break;
        }

        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG_HANDLER, "WIFI_PROV_CRED_SUCCESS");
            break;

        case WIFI_PROV_END:
            xEventGroupSetBits(wifi_events, WIFI_EVENT_BLE_PROV_ENDED);
            ESP_LOGI(TAG_HANDLER, "WIFI_PROV_END");
            break;

        default:
            break;
        }
    }
}

static void wifi_run_ble()
{

    wifi_prov_mgr_config_t prov_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE};
    wifi_prov_mgr_init(prov_config);
    wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, POP, service_name, NULL);
}
static void wifi_stop_ble()
{
    // Stop the provisioning service (disconnects clients)
    wifi_prov_mgr_stop_provisioning();
    // Deinitialize the provisioning manager and free memory
    wifi_prov_mgr_deinit();
}

static void wifi_run_ble_prov_worker(void *params)
{
    TaskHandle_t parent = reinterpret_cast<TaskHandle_t>(params);
    wifi_run_ble();
    xTaskNotifyGive(parent);
    vTaskDelete(NULL);
}

static void wifi_stop_ble_prov_worker(void *params)
{
    TaskHandle_t parent = reinterpret_cast<TaskHandle_t>(params);
    wifi_stop_ble();
    xTaskNotifyGive(parent);
    vTaskDelete(NULL);
}

static void wifi_init_worker(void *params)
{
    TaskHandle_t parent = reinterpret_cast<TaskHandle_t>(params);
    // Initialize NVS
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing TCP/IP adapter...");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "Initializing event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "Initializing Wi-Fi...");
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif)
    {
        ESP_LOGI(TAG, "Creating default Wi-Fi station interface...");
        esp_netif_create_default_wifi_sta();
    }

    ESP_LOGI(TAG, "Registering Wi-Fi and IP event handlers...");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    snprintf(service_name, sizeof(service_name), "PROV_%04X", (uint16_t)esp_random());

    esp_wifi_start();

    xTaskNotifyGive(parent);
    vTaskDelete(NULL);
}

static void wifi_deinit_worker(void *params)
{
    TaskHandle_t parent = reinterpret_cast<TaskHandle_t>(params);

    esp_wifi_stop();

    ESP_LOGI(TAG, "Unregistering Wi-Fi and IP event handlers...");
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler));

    ESP_LOGI(TAG, "Stopping and deinitializing Wi-Fi...");
    esp_wifi_stop();   // stop Wi-Fi (safe to call even if not started)
    esp_wifi_deinit(); // deinitialize Wi-Fi driver

    ESP_LOGI(TAG, "Destroying default netif (if exists)...");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif)
    {
        esp_netif_destroy(netif); // destroy default station interface
    }

    ESP_LOGI(TAG, "Deleting default event loop...");
    ESP_ERROR_CHECK(esp_event_loop_delete_default());

    ESP_LOGI(TAG, "Deinitializing TCP/IP stack...");
    esp_netif_deinit();

    ESP_LOGI(TAG, "Deinitializing NVS...");
    ESP_ERROR_CHECK(nvs_flash_deinit());

    xTaskNotifyGive(parent);
    vTaskDelete(NULL);
}