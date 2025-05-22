#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#ifdef __cplusplus
extern "C"
{
#endif

    enum class Wifi_conn_state
    {
        not_initialized,
        disconnected,
        connecting,
        connected,
    };

    enum class Wifi_con_method
    {
        none,
        saved_credentials,
        ble_provisioning,
    };

    void wifi_manager_trigger_init();
    void wifi_manager_trigger_deinit();
    void wifi_manager_trigger_connect();
    void wifi_manager_trigger_disconnect();
    void wifi_manager_task(void *params);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROV_H