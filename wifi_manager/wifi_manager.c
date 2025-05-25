#include <stdio.h>
#include <string.h> // Cho strncpy
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"         // Thêm include cho event handler unregister
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h" // Có thể cần cho BaseType_t trong xQueueSend
#include "freertos/queue.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

// Bao gồm header chứa định nghĩa sự kiện hệ thống
#include "box_system.h"
#include "event_pool_manager.h"

#define TAG_WM "WIFI_MANAGER"

#define SSID_FORMAT "%s"
#define SSID_STR(s) (char *)(s)

// Biến static để lưu queue handle và trạng thái provisioning
static QueueHandle_t g_master_event_queue_ptr_wm = NULL;
static bool prov_is_running = false; // Cờ theo dõi trạng thái provisioning

//static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err);
//static esp_err_t root_handler(httpd_req_t *req);

static bool is_connecting_on_boot_wm = true; // Đổi tên biến static để tránh xung đột
static esp_timer_handle_t connect_timeout_timer_wm = NULL; // Đổi tên biến static
#define WIFI_CONNECT_TIMEOUT_MS_WM (15 * 1000)
static bool wifi_initialized_wm = false; // Đổi tên biến static
static int s_wifi_retry_count_wm = 0; // Đổi tên biến static
#define MAX_WIFI_RETRIES_WM 3 // Ví dụ, giới hạn retry từ wifi_manager


// Khai báo các hàm static nội bộ
static void connect_timeout_callback_wm(void* arg);
static void wifi_event_handler_internal_wm(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void ip_event_handler_internal_wm(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_prov_event_handler_internal_wm(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);


//=======================================================================================
//Auto connect Wf
//=======================================================================================
// Hàm callback cho timer timeout kết nối

static void connect_timeout_callback_wm(void* arg) {
    ESP_LOGW(TAG_WM, "WiFi connection attempt timed out by wifi_manager timer.");
    if (is_connecting_on_boot_wm) {
        is_connecting_on_boot_wm = false;

        if (g_master_event_queue_ptr_wm != NULL) {
            system_event_t* acquired_event = event_pool_acquire_buffer(0);
            if (acquired_event) {
                acquired_event->type = EVENT_TYPE_WIFI_CONNECT_FAIL;
                acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                // acquired_event->data.wifi.status = WIFI_PROVISION_FAIL; // Không cần thiết cho CONNECT_FAIL
                if (xQueueSend(g_master_event_queue_ptr_wm, &acquired_event, 0) != pdTRUE) {
                    ESP_LOGE(TAG_WM, "Failed to send TIMEOUT WIFI_CONNECT_FAIL event");
                    event_pool_release_buffer(acquired_event);
                } else {
                    ESP_LOGI(TAG_WM, "Sent WIFI_CONNECT_FAIL event due to TIMEOUT.");
                }
            } else {
                ESP_LOGE(TAG_WM, "Failed to acquire event buffer for TIMEOUT WIFI_CONNECT_FAIL event");
            }
        }
    } else {
        ESP_LOGW(TAG_WM, "Connect timeout callback skipped sending event (not in initial boot connect).");
    }
}
esp_err_t wifi_manager_init(QueueHandle_t app_event_queue) {
    if (app_event_queue == NULL) {
        ESP_LOGE(TAG_WM, "Application event queue is NULL!");
        return ESP_ERR_INVALID_ARG;
    }
    g_master_event_queue_ptr_wm = app_event_queue;
    is_connecting_on_boot_wm = true;
    s_wifi_retry_count_wm = 0;

    // NVS, Netif, Event Loop, WiFi Init đã được thực hiện trong main.c (initialize_app)
    // Chỉ đăng ký handler ở đây
    esp_err_t reg_err_wifi = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler_internal_wm, NULL);
    ESP_LOGW(TAG_WM, "Registering wifi_event_handler_internal_wm for WIFI_EVENT returned: %s (0x%x)", esp_err_to_name(reg_err_wifi), reg_err_wifi);
    if (reg_err_wifi != ESP_OK) return reg_err_wifi;

    esp_err_t reg_err_ip = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler_internal_wm, NULL);
    ESP_LOGW(TAG_WM, "Registering ip_event_handler_internal_wm for IP_EVENT_STA_GOT_IP returned: %s (0x%x)", esp_err_to_name(reg_err_ip), reg_err_ip);
    if (reg_err_ip != ESP_OK) return reg_err_ip;
    
    wifi_initialized_wm = true;
    ESP_LOGI(TAG_WM, "WiFi Manager initialized successfully.");
    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(void) {
    if (!wifi_initialized_wm) {
        ESP_LOGE(TAG_WM, "WiFi Manager not initialized. Cannot connect.");
        return ESP_ERR_INVALID_STATE;
    }
    if (prov_is_running) {
        ESP_LOGW(TAG_WM, "Provisioning is running. Cannot start STA connection.");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_cfg;
    esp_err_t get_config_err = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    if (get_config_err != ESP_OK || strlen((char*)wifi_cfg.sta.ssid) == 0) {
        if (get_config_err != ESP_OK) { ESP_LOGW(TAG_WM, "Failed to get stored WiFi config (%s). Starting provisioning.", esp_err_to_name(get_config_err)); }
        else { ESP_LOGW(TAG_WM, "Stored WiFi config SSID is empty. Starting provisioning."); }
        
        if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
        is_connecting_on_boot_wm = false;
        return start_wifi_provisioning(); // Hàm này cũng cần dùng event pool
    }

    ESP_LOGI(TAG_WM, "Stored WiFi config found. SSID: %s", wifi_cfg.sta.ssid);

    if (connect_timeout_timer_wm == NULL) {
         const esp_timer_create_args_t timer_args = { .callback = &connect_timeout_callback_wm, .name = "wifi_conn_timeout_wm" };
         esp_err_t timer_err = esp_timer_create(&timer_args, &connect_timeout_timer_wm);
         if (timer_err != ESP_OK) { ESP_LOGE(TAG_WM, "Failed to create conn timeout timer: %s", esp_err_to_name(timer_err)); return timer_err; }
    }
    esp_timer_start_once(connect_timeout_timer_wm, WIFI_CONNECT_TIMEOUT_MS_WM * 1000);

    is_connecting_on_boot_wm = true;
    s_wifi_retry_count_wm = 0;
    ESP_LOGI(TAG_WM, "Setting is_connecting_on_boot_wm = true before STA connection attempt.");

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WM, "Failed to set WiFi mode STA: %s", esp_err_to_name(err));
        if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
        is_connecting_on_boot_wm = false;
        return err;
    }
    // esp_wifi_start() đã được gọi trong initialize_app của main.c
    // Nếu gọi lại, nó sẽ trả về ESP_ERR_WIFI_STARTED, không phải lỗi.
    // err = esp_wifi_start();
    // if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_STARTED) {
    //     ESP_LOGE(TAG_WM, "Failed to start WiFi stack: %s", esp_err_to_name(err));
    //     if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
    //     is_connecting_on_boot_wm = false;
    //     return err;
    // }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WM, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
        is_connecting_on_boot_wm = false;
        return err;
    }

    ESP_LOGI(TAG_WM, "WiFi STA connection attempt initiated successfully.");
    return ESP_OK;
}

static void wifi_event_handler_internal_wm(void* arg, esp_event_base_t event_base,
                                           int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_WM, "WIFI_EVENT_STA_START received. (Connection initiated by wifi_manager_connect_sta)");
        // esp_wifi_connect(); // Không cần gọi lại ở đây, đã gọi trong wifi_manager_connect_sta
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG_WM, "WiFi Disconnected. Reason: %d. is_connecting_on_boot_wm: %d", disconnected->reason, is_connecting_on_boot_wm);

        if (is_connecting_on_boot_wm) { // Lỗi trong quá trình kết nối ban đầu
            is_connecting_on_boot_wm = false;
            if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
            ESP_LOGE(TAG_WM, "Initial connection failed due to STA_DISCONNECTED.");

            if (g_master_event_queue_ptr_wm != NULL) {
                system_event_t* acquired_event = event_pool_acquire_buffer(0);
                if (acquired_event) {
                    acquired_event->type = EVENT_TYPE_WIFI_CONNECT_FAIL;
                    acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                    // acquired_event->data.wifi.status = WIFI_PROVISION_FAIL; // Không đúng ngữ cảnh
                    // Gán mã lỗi vào event data nếu cần
                    // acquired_event->data.wifi.error_code = disconnected->reason;

                    if (xQueueSend(g_master_event_queue_ptr_wm, &acquired_event, 0) != pdPASS) {
                        ESP_LOGE(TAG_WM, "Failed to send WIFI_CONNECT_FAIL event! Releasing buffer.");
                        event_pool_release_buffer(acquired_event);
                    } else {
                        ESP_LOGI(TAG_WM, "Sent WIFI_CONNECT_FAIL event (from pool) to master queue.");
                    }
                } else { ESP_LOGE(TAG_WM, "Failed to acquire event buffer for WIFI_CONNECT_FAIL!"); }
            }
        } else { // Mất kết nối trong quá trình hoạt động bình thường
            ESP_LOGW(TAG_WM, "Lost WiFi connection (normal operation).");
            if (g_master_event_queue_ptr_wm != NULL) {
                system_event_t* acquired_event = event_pool_acquire_buffer(0);
                if (acquired_event) {
                    acquired_event->type = EVENT_TYPE_WIFI_DISCONNECTED; // Event riêng cho mất kết nối
                    acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                    if (xQueueSend(g_master_event_queue_ptr_wm, &acquired_event, 0) != pdPASS) {
                        ESP_LOGE(TAG_WM, "Failed to send WIFI_DISCONNECTED event! Releasing buffer.");
                        event_pool_release_buffer(acquired_event);
                    } else {
                        ESP_LOGI(TAG_WM, "Sent WIFI_DISCONNECTED event (from pool) to master queue.");
                    }
                } else { ESP_LOGE(TAG_WM, "Failed to acquire event buffer for WIFI_DISCONNECTED!"); }
            }
        }
    }
}

static void ip_event_handler_internal_wm(void* arg, esp_event_base_t event_base,
                                         int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WM, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));

        if (is_connecting_on_boot_wm) {
            ESP_LOGI(TAG_WM, "Initial connection successful!");
            if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
        }
        // is_connecting_on_boot_wm sẽ được set false bởi state machine hoặc khi timeout/disconnect
        // Ở đây, chỉ cần biết là đã có IP.
        s_wifi_retry_count_wm = 0;

        if (g_master_event_queue_ptr_wm != NULL) {
            system_event_t* acquired_event = event_pool_acquire_buffer(pdMS_TO_TICKS(100));
            if (acquired_event) {
                acquired_event->type = EVENT_TYPE_WIFI_CONNECTED;
                acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                // Nếu cần gửi IP:
                // strncpy(acquired_event->data.wifi_connected_info.ip_addr, ip4addr_ntoa(&event->ip_info.ip), sizeof(acquired_event->data.wifi_connected_info.ip_addr)-1);

                if (xQueueSend(g_master_event_queue_ptr_wm, &acquired_event, pdMS_TO_TICKS(100)) != pdPASS) {
                    ESP_LOGE(TAG_WM, "Failed to send WIFI_CONNECTED event! Releasing buffer.");
                    event_pool_release_buffer(acquired_event);
                } else {
                    ESP_LOGI(TAG_WM, "Sent WIFI_CONNECTED event (from pool) to master queue.");
                }
            } else {
                ESP_LOGE(TAG_WM, "Failed to acquire event buffer for WIFI_CONNECTED event!");
            }
        }
        ESP_LOGW(TAG_WM, "Processing GOT_IP, is_connecting_on_boot_wm was 1 -> %d (after processing)", is_connecting_on_boot_wm); // Log trạng thái cờ
        is_connecting_on_boot_wm = false; // Đặt lại cờ sau khi đã xử lý GOT_IP và gửi event
    }
}


esp_err_t start_wifi_provisioning(void) {
    if (!wifi_initialized_wm) { ESP_LOGE(TAG_WM, "WiFi manager not initialized for provisioning"); return ESP_ERR_INVALID_STATE; }
    if (prov_is_running) { ESP_LOGW(TAG_WM, "Provisioning is already running."); return ESP_OK; } // Không lỗi nếu đã chạy

    ESP_LOGI(TAG_WM, "Attempting to start WiFi provisioning...");

    if (is_connecting_on_boot_wm) {
        ESP_LOGW(TAG_WM, "Stopping initial STA connection attempt for provisioning.");
        is_connecting_on_boot_wm = false;
        if (connect_timeout_timer_wm != NULL) { esp_timer_stop(connect_timeout_timer_wm); }
    }
    
    // Dừng WiFi STA mode nếu đang chạy để chuyển sang APSTA cho provisioning
    // esp_wifi_disconnect(); // Dừng mọi nỗ lực kết nối STA
    esp_err_t stop_err = esp_wifi_stop(); // Dừng stack WiFi
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG_WM, "Failed to stop WiFi before provisioning: %s", esp_err_to_name(stop_err));
        // return stop_err; // Có thể không cần return lỗi ngay, thử tiếp
    } else {
        ESP_LOGI(TAG_WM, "WiFi stack stopped for provisioning setup.");
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Chờ một chút

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config)); // Dùng ERROR_CHECK nếu init là critical

    // Đăng ký handler cho provisioning events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler_internal_wm, NULL));

    char service_name[16];
    uint32_t random_val = esp_random();
    snprintf(service_name, sizeof(service_name), "BOX-%05lX", random_val & 0xFFFFF);

    esp_err_t prov_start_err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, NULL, service_name, NULL);
    if (prov_start_err != ESP_OK) {
        ESP_LOGE(TAG_WM, "Failed to start provisioning: %s", esp_err_to_name(prov_start_err));
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler_internal_wm);
        wifi_prov_mgr_deinit();
        return prov_start_err;
    }

    prov_is_running = true;
    ESP_LOGI(TAG_WM, "WiFi provisioning started with service name: %s", service_name);

    // Gửi event yêu cầu UI cho provisioning (nếu main task cần biết)
    if (g_master_event_queue_ptr_wm != NULL) {
        system_event_t* ui_req_event = event_pool_acquire_buffer(0);
        if (ui_req_event) {
            ui_req_event->type = EVENT_TYPE_PROVISIONING_REQUEST; // Hoặc một event như EVENT_TYPE_PROV_UI_SHOW
            ui_req_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
            if (xQueueSend(g_master_event_queue_ptr_wm, &ui_req_event, 0) != pdPASS) {
                ESP_LOGE(TAG_WM, "Failed to send PROV_UI_REQUEST event.");
                event_pool_release_buffer(ui_req_event);
            }
        } else { ESP_LOGE(TAG_WM, "Failed to acquire buffer for PROV_UI_REQUEST event."); }
    }
    return ESP_OK;
}

esp_err_t stop_wifi_provisioning(void) {
    if (!prov_is_running) {
        ESP_LOGW(TAG_WM, "Provisioning is not running, nothing to stop.");
        return ESP_OK;
    }
    ESP_LOGI(TAG_WM, "Stopping WiFi provisioning...");
    wifi_prov_mgr_stop_provisioning();
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler_internal_wm);
    wifi_prov_mgr_deinit();
    prov_is_running = false;

    // Sau khi dừng provisioning, thử kết nối lại với cấu hình đã lưu (nếu có)
    // Hoặc để main task quyết định hành động tiếp theo.
    // ESP_LOGI(TAG_WM, "Attempting to connect to STA after provisioning stop.");
    // wifi_manager_connect_sta(); // Có thể gọi ở đây hoặc để main task gọi
    return ESP_OK;
}

static void wifi_prov_event_handler_internal_wm(void* arg, esp_event_base_t event_base,
                                                int32_t event_id, void* event_data) {
    if (event_base != WIFI_PROV_EVENT) return;

    system_event_t* acquired_event = event_pool_acquire_buffer(pdMS_TO_TICKS(50));
    if (!acquired_event) {
        ESP_LOGE(TAG_WM, "Failed to acquire event buffer for provisioning event %ld", event_id);
        return;
    }
    acquired_event->type = EVENT_TYPE_WIFI_PROVISION;
    acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
    bool send_event = true;

    switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG_WM, "Provisioning started by manager.");
            acquired_event->data.wifi.status = WIFI_PROVISION_START;
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG_WM, "Provisioning: Received Wi-Fi credentials for SSID: %s", (const char*)wifi_sta_cfg->ssid);
            send_event = false; // Không gửi event này, chờ SUCCESS/FAIL
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG_WM, "Provisioning credential failed! Reason: %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Auth Error" : "AP Not Found");
            acquired_event->data.wifi.status = WIFI_PROVISION_FAIL;
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG_WM, "Provisioning credential successful!");
            acquired_event->data.wifi.status = WIFI_PROVISION_SUCCESS;
            // Lấy SSID đã lưu và gửi kèm event
            wifi_config_t wifi_cfg_temp;
            if(esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg_temp) == ESP_OK) {
                strncpy(acquired_event->data.wifi.ssid, (const char*)wifi_cfg_temp.sta.ssid, sizeof(acquired_event->data.wifi.ssid)-1);
            }
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG_WM, "Provisioning ended by manager.");
            // Không cần thiết phải deinit ở đây, stop_wifi_provisioning sẽ làm
            send_event = false; // Không gửi event này
            break;
        default:
            ESP_LOGW(TAG_WM, "Unhandled provisioning event: %ld", event_id);
            send_event = false;
            break;
    }

    if (send_event) {
        if (g_master_event_queue_ptr_wm != NULL) {
            if (xQueueSend(g_master_event_queue_ptr_wm, &acquired_event, 0) != pdPASS) {
                ESP_LOGE(TAG_WM, "Failed to send provisioning event %ld to master queue! Releasing buffer.", event_id);
                event_pool_release_buffer(acquired_event);
            } else {
                ESP_LOGI(TAG_WM, "Sent provisioning event %ld (from pool) to master queue.", event_id);
            }
        } else {
            ESP_LOGE(TAG_WM, "Master queue is NULL! Releasing buffer for prov event %ld.", event_id);
            event_pool_release_buffer(acquired_event);
        }
    } else if (acquired_event) { // Nếu không gửi event, phải release buffer đã acquire
        event_pool_release_buffer(acquired_event);
    }
}

esp_err_t wifi_manager_deinit(void) {
    // (Giữ nguyên logic deinit của bạn, đảm bảo dừng timer, unregister handler)
    if (prov_is_running) { stop_wifi_provisioning(); }
    esp_wifi_stop();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler_internal_wm);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler_internal_wm);
    // esp_wifi_deinit(); // Deinit stack WiFi nên ở app_main nếu không còn dùng nữa
    if (connect_timeout_timer_wm != NULL) { esp_timer_delete(connect_timeout_timer_wm); connect_timeout_timer_wm = NULL; }
    g_master_event_queue_ptr_wm = NULL;
    wifi_initialized_wm = false;
    is_connecting_on_boot_wm = false;
    ESP_LOGI(TAG_WM, "WiFi Manager de-initialized.");
    return ESP_OK;
}