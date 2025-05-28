#include <stdio.h>
#include "rtdb_client.h"
#include "box_system.h"     // Để có định nghĩa EVENT_TYPE_...
#include "event_pool_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"  

#include "nvs.h"
#include "nvs_flash.h"



static const char *TAG = "RTDB_CLIENT";




static char s_id_token[MAX_ID_TOKEN_LEN_INTERNAL] = {0};
static char s_refresh_token[MAX_REFRESH_TOKEN_LEN_INTERNAL] = {0};

static char s_uid[MAX_UID_LEN_INTERNAL] = {0};
static char s_web_api_key[MAX_API_KEY_LEN_INTERNAL] = {0};

// (Giữ nguyên http_event_handler_user_data_t và _http_event_handler_cb như trước,
//  nhưng _http_event_handler_cb sẽ được mở rộng)

typedef struct {
    char *response_buffer_http; // Buffer động cho toàn bộ response HTTP
    int response_buffer_http_size;
    int current_response_http_len;
    system_event_type_t event_type_on_success;
    // Không cần result_data_for_event nữa, vì sẽ điền trực tiếp vào event từ pool
    char requested_path_for_get[RTDB_PATH_MAX_LEN]; // Giữ lại để truyền path
    bool auth_in_url;   // Cờ để xác định xem có cần thêm token vào URL không
    // Thêm các context khác nếu cần
} http_event_handler_user_data_t;



// Khai báo static cho hàm helper nội bộ
static esp_err_t _http_event_handler_cb(esp_http_client_event_t *evt);
static void http_request_task(void *pvParameters);
static esp_err_t rtdb_client_create_http_task(const char* url_str, esp_http_client_method_t method, // << SỬA Ở ĐÂY
                                             system_event_type_t success_event_type,
                                             const char* post_payload,
                                             const char* requested_path);

static esp_err_t _start_anonymous_signin(void);
static esp_err_t _start_token_refresh(void); // Sẽ dùng ở bước sau



//Hàm helper để đọc và ghi chuỗi vào NVS, hàm này sẽ được gọi từ các hàm khác trong rtdb_client.c
//static esp_err_t _nvs_read_string(nvs_handle_t nvs_handle_param, const char* key, char* out_value, size_t max_len);
esp_err_t _nvs_read_string(nvs_handle_t nvs_handle_param, const char* key, char* out_value, size_t max_len) {
    if (!out_value || max_len == 0) return ESP_ERR_INVALID_ARG;
    out_value[0] = '\0'; // Khởi tạo chuỗi rỗng
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvs_handle_param, key, NULL, &required_size);
    if (err == ESP_OK && required_size > 0) {
        if (required_size <= max_len) {
            err = nvs_get_str(nvs_handle_param, key, out_value, &required_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "NVS: Failed to get string for key '%s' (%s)", key, esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "NVS: Buffer too small for key '%s'. Required: %zu, Max: %zu", key, required_size, max_len);
            err = ESP_ERR_NVS_INVALID_LENGTH;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS: Key '%s' not found.", key);
        // Không phải lỗi, chỉ là chưa có giá trị
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: Error getting size for key '%s' (%s)", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t _nvs_write_string(nvs_handle_t nvs_handle_param, const char* key, const char* value) {
    if (!value) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_set_str(nvs_handle_param, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: Failed to set string for key '%s' (%s)", key, esp_err_to_name(err));
    }
    return err;
}

// Hàm này sẽ được gọi từ task authentication_worker_task, được gọi trong trường hợp cần xác thực thiết bị khi đã có thông tin xác thực trong NVS
static esp_err_t _start_token_refresh() {
    if (strlen(s_web_api_key) == 0) {
        ESP_LOGE(TAG, "Web API Key not available for token refresh.");
        return ESP_ERR_INVALID_STATE;
    }
    // Quan trọng: s_refresh_token phải được load từ NVS trước khi gọi hàm này
    if (strlen(s_refresh_token) == 0) {
        ESP_LOGE(TAG, "Refresh token not available in RAM for token refresh. (Was it loaded from NVS?)");
        return ESP_ERR_INVALID_STATE; // Sẽ khiến logic trong authenticate_device chuyển sang anonymous sign-in
    }


    char refresh_url[256];
    snprintf(refresh_url, sizeof(refresh_url), "https://securetoken.googleapis.com/v1/token?key=%s", s_web_api_key);

    // Payload cho yêu cầu làm mới token - KHÔNG CHUẨN BỊ PAYLOAD ở đây, 
    //char refresh_payload[4096]; // Đảm bảo đủ lớn
    //snprintf(refresh_payload, sizeof(refresh_payload),"{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}",s_refresh_token);
    //ESP_LOGI(TAG, "_start_token_refresh: Using payload: %s", refresh_payload);

    // Sử dụng lại event type EVENT_TYPE_FIREBASE_AUTH_RESULT để xử lý kết quả
    // Hàm _http_event_handler_cb sẽ parse response và cập nhật token mới.
    return rtdb_client_create_http_task(refresh_url, HTTP_METHOD_POST,
                                         EVENT_TYPE_FIREBASE_AUTH_RESULT,
                                         NULL, // PAYLOAD sẽ được chuẩn bị trong task
                                         NULL); // requested_path (không áp dụng)
}


// Khai báo task handle (nếu bạn muốn có khả năng quản lý task từ bên ngoài, không bắt buộc)
// static TaskHandle_t rtdb_auth_task_handle = NULL;

// Hàm thực hiện logic xác thực, sẽ được gọi trong một task riêng
static void rtdb_authentication_worker_task(void *pvParameter) {
    ESP_LOGI(TAG, "rtdb_authentication_worker_task started.");
    nvs_handle_t nvs_handle;
    esp_err_t nvs_err;
    bool attempt_refresh = false;

    // API key đã được lưu vào s_web_api_key bởi rtdb_client_authenticate_device trước khi tạo task này.
    if (strlen(s_web_api_key) == 0) {
        ESP_LOGE(TAG, "Auth Task: Web API Key is NOT available. Task exiting.");
        // Gửi event lỗi về state machine nếu cần thiết
        // system_event_t* err_evt = event_pool_acquire_buffer(0);
        // if (err_evt) {
        //    err_evt->type = EVENT_TYPE_FIREBASE_AUTH_RESULT;
        //    err_evt->data.firebase_auth_result.status = ESP_FAIL;
        //    err_evt->data.firebase_auth_result.http_status_code = -1; // Lỗi nội bộ
        //    strncpy(err_evt->data.firebase_auth_result.error_message, "API Key Missing", RTDB_ERROR_MSG_MAX_LEN-1);
        //    // Gửi event...
        //    // event_pool_release_buffer(err_evt); // nếu gửi lỗi
        // }
        vTaskDelete(NULL);
        return;
    }

    // 1. Thử tải Refresh Token và UID từ NVS
    ESP_LOGI(TAG, "Auth Task: Attempting to load refresh token and UID from NVS...");
    nvs_err = nvs_open(NVS_NAMESPACE_AUTH, NVS_READONLY, &nvs_handle);
    if (nvs_err == ESP_OK) {
        _nvs_read_string(nvs_handle, NVS_KEY_REFRESH_TOKEN, s_refresh_token, MAX_REFRESH_TOKEN_LEN_INTERNAL);
        _nvs_read_string(nvs_handle, NVS_KEY_UID, s_uid, MAX_UID_LEN_INTERNAL);
        _nvs_read_string(nvs_handle, NVS_KEY_ID_TOKEN, s_id_token, MAX_ID_TOKEN_LEN_INTERNAL);
        // Lưu ý: Không cần đọc API Key từ NVS nữa, vì đã có s_web_api_key từ bên ngoài
        // Nếu bạn cần đọc API Key từ NVS, hãy thêm vào đây
        // _nvs_read_string(nvs_handle, NVS_KEY_API_KEY, s_web_api_key, MAX_API
        
        nvs_close(nvs_handle);

        if (strlen(s_refresh_token) > 0 && strlen(s_uid) > 0) {
            ESP_LOGI(TAG, "Auth Task: Refresh token and UID loaded from NVS.");
            ESP_LOGI(TAG, "Auth Task: NVS Loaded UID: %s", s_uid);
            ESP_LOGI(TAG, "Auth Task: NVS Loaded Refresh Token: %.20s...", s_refresh_token);
            attempt_refresh = true;
        } else {
            ESP_LOGI(TAG, "Auth Task: No valid refresh token or UID found in NVS.");
            memset(s_id_token, 0, sizeof(s_id_token)); // Xóa id_token RAM (nếu có từ lần chạy trước)
            memset(s_refresh_token, 0, sizeof(s_refresh_token));
            memset(s_uid, 0, sizeof(s_uid));
        }
    } else {
        ESP_LOGE(TAG, "Auth Task: Failed to open NVS to read tokens (%s). Assuming no tokens.", esp_err_to_name(nvs_err));
        memset(s_id_token, 0, sizeof(s_id_token));
        memset(s_refresh_token, 0, sizeof(s_refresh_token));
        memset(s_uid, 0, sizeof(s_uid));
    }

    // 2. Quyết định: Làm mới Token hay Đăng nhập Ẩn danh Mới
    esp_err_t auth_action_err = ESP_FAIL;
    if (attempt_refresh) {
        memset(s_id_token, 0, sizeof(s_id_token)); // Xóa id_token cũ trong RAM trước khi làm mới
        ESP_LOGI(TAG, "Auth Task: Proceeding to token refresh.");
        auth_action_err = _start_token_refresh(); // Hàm này tạo HTTP task
    } else {
        ESP_LOGI(TAG, "Auth Task: Proceeding with new anonymous sign-in.");
        auth_action_err = _start_anonymous_signin(); // Hàm này tạo HTTP task
    }

    if (auth_action_err != ESP_OK) {
        ESP_LOGE(TAG, "Auth Task: Failed to start authentication action (refresh/anonymous): %s", esp_err_to_name(auth_action_err));
        // Gửi event lỗi về state machine nếu việc *khởi tạo* action bị lỗi
        system_event_t* err_evt = event_pool_acquire_buffer(0);
        if (err_evt) {
           err_evt->type = EVENT_TYPE_FIREBASE_AUTH_RESULT; // Sử dụng cùng loại event
           err_evt->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
           err_evt->data.firebase_auth_result.status = ESP_FAIL; // Lỗi
           err_evt->data.firebase_auth_result.http_status_code = -1; // Mã lỗi nội bộ client
           snprintf(err_evt->data.firebase_auth_result.error_message, RTDB_ERROR_MSG_MAX_LEN, "Auth action init fail: %s", esp_err_to_name(auth_action_err));
           if (g_master_event_queue_ptr && xQueueSend(g_master_event_queue_ptr, &err_evt, 0) != pdPASS) {
               ESP_LOGE(TAG, "Auth Task: Failed to send auth action init fail event.");
               event_pool_release_buffer(err_evt);
           }
           // Nếu không gửi được, buffer sẽ được giải phóng nếu acquire thành công
        } else {
            ESP_LOGE(TAG, "Auth Task: Failed to acquire buffer for auth action init fail event.");
        }
    } else {
        ESP_LOGI(TAG, "Auth Task: Authentication action (refresh/anonymous) initiated successfully.");
        // HTTP task tương ứng sẽ gửi EVENT_TYPE_FIREBASE_AUTH_RESULT khi hoàn thành.
    }

    ESP_LOGI(TAG, "rtdb_authentication_worker_task finished its work, deleting self.");
    // rtdb_auth_task_handle = NULL; // Nếu bạn dùng task handle
    vTaskDelete(NULL); // Task tự xóa sau khi đã khởi tạo hành động xác thực (HTTP task)
}



/*
    bool api_key_available = false;


    if (web_api_key_param && strlen(web_api_key_param) > 0) {
        if (strlen(web_api_key_param) < MAX_API_KEY_LEN_INTERNAL) {
            strncpy(s_web_api_key, web_api_key_param, MAX_API_KEY_LEN_INTERNAL - 1);
            s_web_api_key[MAX_API_KEY_LEN_INTERNAL - 1] = '\0';
            ESP_LOGI(TAG, "Using API key from parameter and saving to NVS.");

            nvs_handle_t nvs_handle;
            esp_err_t nvs_err = nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &nvs_handle);
            if (nvs_err == ESP_OK) {
                _nvs_write_string(nvs_handle, NVS_KEY_API_KEY, s_web_api_key);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
            } else {
                ESP_LOGE(TAG, "Failed to open NVS to save API key (%s)", esp_err_to_name(nvs_err));
            }
            api_key_available = true;
        } else {
            ESP_LOGE(TAG, "Web API Key parameter is too long.");
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        ESP_LOGI(TAG, "No API key from parameter, trying to load from NVS...");
        nvs_handle_t nvs_handle;
        esp_err_t nvs_err = nvs_open(NVS_NAMESPACE_AUTH, NVS_READONLY, &nvs_handle);
        if (nvs_err == ESP_OK) {
            esp_err_t read_err = _nvs_read_string(nvs_handle, NVS_KEY_API_KEY, s_web_api_key, MAX_API_KEY_LEN_INTERNAL);
            if (read_err == ESP_OK && strlen(s_web_api_key) > 0) {
                ESP_LOGI(TAG, "API key loaded from NVS.");
                api_key_available = true;
            } else {
                ESP_LOGW(TAG, "Failed to read API key from NVS or key is empty.");
            }
            nvs_close(nvs_handle);
        } else {
            ESP_LOGE(TAG, "Failed to open NVS to read API key (%s)", esp_err_to_name(nvs_err));
        }
    }

    if (!api_key_available) {
        ESP_LOGE(TAG, "Web API Key is not available. Cannot proceed with authentication.");
        return ESP_ERR_INVALID_STATE;
    }

    // Xóa thông tin xác thực cũ trong RAM (UID, ID Token, Refresh Token)
    memset(s_uid, 0, sizeof(s_uid));
    memset(s_id_token, 0, sizeof(s_id_token));
    memset(s_refresh_token, 0, sizeof(s_refresh_token));

    // Bước 1: Tạm thời chỉ thực hiện anonymous sign-in
    ESP_LOGI(TAG, "Step 1 Test: Proceeding with new anonymous sign-in.");
    return _start_anonymous_signin();
} // end of rtdb_client_authenticate_device
*/

// Hàm _start_anonymous_signin (payload phải đúng)
static esp_err_t _start_anonymous_signin() {
    if (strlen(s_web_api_key) == 0) {
        ESP_LOGE(TAG, "Web API Key not available for anonymous sign-in.");
        // Nên gửi event lỗi về main task
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Starting new anonymous sign-in...");
    //const char *post_payload_anon = "{\"returnSecureToken\":true}"; // Đảm bảo JSON này đúng
    //ESP_LOGI(TAG, "_start_anonymous_signin: Using payload: %s", post_payload_anon);
    
    char auth_url[256];
    snprintf(auth_url, sizeof(auth_url), "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=%s", s_web_api_key);

    // Gọi hàm test httpbin nếu bạn muốn giữ lại cho bước này
    #ifdef ENABLE_HTTPBIN_ANON_TEST
    _rtdb_client_test_post_payload_to_httpbin(post_payload_anon, "ANON_SIGNIN_STEP1");
    #endif

    return rtdb_client_create_http_task(auth_url, HTTP_METHOD_POST, EVENT_TYPE_FIREBASE_AUTH_RESULT, NULL, NULL);
    //return rtdb_client_create_http_task(auth_url, HTTP_METHOD_POST, EVENT_TYPE_FIREBASE_AUTH_RESULT, post_payload_anon, NULL);
}


static esp_err_t _http_event_handler_cb(esp_http_client_event_t *evt) {
    http_event_handler_user_data_t *user_data_from_event = (http_event_handler_user_data_t *)evt->user_data;
    system_event_t* acquired_event = NULL;
    char url_buffer[128];

    if (user_data_from_event == NULL) { ESP_LOGE(TAG, "HTTP Handler: user_data is NULL at event_id %d", evt->event_id); return ESP_FAIL; }
    // Không cần kiểm tra user_data->result_data_for_event nữa



    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            // Không cần xử lý result_data_for_event ở đây nữa
            // Nếu cần gửi event lỗi, phải acquire buffer trước
            acquired_event = event_pool_acquire_buffer(pdMS_TO_TICKS(10));
            if (acquired_event) {
                acquired_event->type = user_data_from_event->event_type_on_success; // Dùng cùng type, nhưng status sẽ là lỗi
                acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                if (user_data_from_event->event_type_on_success == EVENT_TYPE_HTTP_TEST_RESULT) {
                    acquired_event->data.http_test_result.status = ESP_FAIL;
                    acquired_event->data.http_test_result.http_status_code = -1;
                } else if (user_data_from_event->event_type_on_success == EVENT_TYPE_FIREBASE_AUTH_RESULT) {
                    acquired_event->data.firebase_auth_result.status = ESP_FAIL;
                    acquired_event->data.firebase_auth_result.http_status_code = -1;
                    strncpy(acquired_event->data.firebase_auth_result.error_message, "HTTP Client Error", RTDB_ERROR_MSG_MAX_LEN -1);
                } else if (user_data_from_event->event_type_on_success == EVENT_TYPE_RTDB_GET_RESULT) {
                    acquired_event->data.rtdb_get_result.status = ESP_FAIL;
                    acquired_event->data.rtdb_get_result.http_status_code = -1;
                    strncpy(acquired_event->data.rtdb_get_result.path, user_data_from_event->requested_path_for_get, RTDB_PATH_MAX_LEN -1);
                }
                // Gửi event lỗi
                if (g_master_event_queue_ptr != NULL) {
                    if (xQueueSend(g_master_event_queue_ptr, &acquired_event, 0) != pdPASS) {
                        ESP_LOGE(TAG, "Failed to send ERROR event to master queue, releasing buffer.");
                        event_pool_release_buffer(acquired_event);
                    }
                } else { event_pool_release_buffer(acquired_event); }
            } else { ESP_LOGE(TAG, "Failed to acquire event buffer for HTTP_EVENT_ERROR"); }
            break;
        case HTTP_EVENT_ON_CONNECTED:
            if (esp_http_client_get_url(evt->client, url_buffer, sizeof(url_buffer)) == ESP_OK) {
                ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED to %s", url_buffer);
            }
            if (user_data_from_event) { // Chỉ kiểm tra user_data_from_event
                // KHÔNG giải phóng user_data_from_event hoặc response_buffer_http ở đây.
                // Chỉ reset buffer để chuẩn bị nhận dữ liệu phản hồi.
                user_data_from_event->current_response_http_len = 0;
                if (user_data_from_event->response_buffer_http) {
                    user_data_from_event->response_buffer_http[0] = '\0';
                }
            }
            break;
        case HTTP_EVENT_ON_DATA:
            // (Giữ nguyên logic on_data, ghi vào user_data->response_buffer_http)
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (user_data_from_event->response_buffer_http && evt->data_len > 0) {
                if (user_data_from_event->current_response_http_len + evt->data_len < user_data_from_event->response_buffer_http_size) {
                    memcpy(user_data_from_event->response_buffer_http + user_data_from_event->current_response_http_len, evt->data, evt->data_len);
                    user_data_from_event->current_response_http_len += evt->data_len;
                    user_data_from_event->response_buffer_http[user_data_from_event->current_response_http_len] = '\0';
                } else {
                    ESP_LOGW(TAG, "HTTP response_buffer_http overflow. Discarding new data.");
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:  //Hoàn thành request, 
            if (esp_http_client_get_url(evt->client, url_buffer, sizeof(url_buffer)) == ESP_OK) {
                ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH for %s", url_buffer);
            }
            int http_status = esp_http_client_get_status_code(evt->client);
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH: Actual HTTP Status Code: %d", http_status);

            acquired_event = event_pool_acquire_buffer(pdMS_TO_TICKS(100)); // Chờ tối đa 100ms
            if (!acquired_event) {
                ESP_LOGE(TAG, "Failed to acquire event buffer on HTTP_FINISH for %s. Data lost.", url_buffer);
                break; // Không thể gửi event
            }

            acquired_event->type = user_data_from_event->event_type_on_success;
            acquired_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);

            if (user_data_from_event->event_type_on_success == EVENT_TYPE_HTTP_TEST_RESULT) {  
                rtdb_http_test_result_data_t* res_data = &acquired_event->data.http_test_result;   
                res_data->http_status_code = http_status;
                if (http_status >= 200 && http_status < 300) {
                    res_data->status = ESP_OK;
                    if (user_data_from_event->response_buffer_http && user_data_from_event->current_response_http_len > 0) {
                        strncpy(res_data->response_data, user_data_from_event->response_buffer_http, RTDB_HTTP_RESPONSE_MAX_LEN - 1);   // Copy response
                        res_data->response_data[RTDB_HTTP_RESPONSE_MAX_LEN - 1] = '\0';
                    } else { res_data->response_data[0] = '\0';}
                } else { res_data->status = ESP_FAIL; res_data->response_data[0] = '\0';}
            }
            else if (user_data_from_event->event_type_on_success == EVENT_TYPE_FIREBASE_AUTH_RESULT) {
                rtdb_firebase_auth_result_data_t* auth_data = &acquired_event->data.firebase_auth_result;
                auth_data->http_status_code = http_status;
                memset(s_id_token, 0, sizeof(s_id_token));
                memset(s_refresh_token, 0, sizeof(s_refresh_token));
                bool parse_success = false;

                if (http_status == 200 && user_data_from_event->response_buffer_http) {
                    ESP_LOGI(TAG, "Raw Firebase Auth Resp: <%s>", user_data_from_event->response_buffer_http);
                    cJSON *json_root = cJSON_Parse(user_data_from_event->response_buffer_http);
                    if (json_root) {
                        bool has_id_token = false;
                        bool has_refresh_token = false;
                        bool has_uid = false;
                        
                        
                        cJSON *item_id_token_val = cJSON_GetObjectItemCaseSensitive(json_root, "id_token");
                        if (!item_id_token_val) {
                            item_id_token_val = cJSON_GetObjectItemCaseSensitive(json_root, "idToken");
                        }
                        if (cJSON_IsString(item_id_token_val) && item_id_token_val->valuestring) {
                            strncpy(auth_data->id_token, item_id_token_val->valuestring, RTDB_ID_TOKEN_MAX_LEN - 1);
                            strncpy(s_id_token, item_id_token_val->valuestring, MAX_ID_TOKEN_LEN_INTERNAL - 1);
                            s_id_token[MAX_ID_TOKEN_LEN_INTERNAL -1] = '\0'; // Null terminate
                            auth_data->id_token[RTDB_ID_TOKEN_MAX_LEN -1] = '\0';
                            has_id_token = true;
                        }

                        cJSON *item_refresh_token_val = cJSON_GetObjectItemCaseSensitive(json_root, "refresh_token");
                        if (!item_refresh_token_val) {
                            item_refresh_token_val = cJSON_GetObjectItemCaseSensitive(json_root, "refreshToken");
                        }
                        if (cJSON_IsString(item_refresh_token_val) && item_refresh_token_val->valuestring) {
                            strncpy(auth_data->refresh_token, item_refresh_token_val->valuestring, RTDB_REFRESH_TOKEN_MAX_LEN - 1);
                            strncpy(s_refresh_token, item_refresh_token_val->valuestring, MAX_REFRESH_TOKEN_LEN_INTERNAL - 1);
                            s_refresh_token[MAX_REFRESH_TOKEN_LEN_INTERNAL -1] = '\0'; // Null terminate
                            auth_data->refresh_token[RTDB_REFRESH_TOKEN_MAX_LEN -1] = '\0';
                            has_refresh_token = true;
                        }
                        cJSON *item_uid_val = cJSON_GetObjectItemCaseSensitive(json_root, "user_id");
                        if (!item_uid_val) {
                            item_uid_val = cJSON_GetObjectItemCaseSensitive(json_root, "localId");
                        }
                        if (cJSON_IsString(item_uid_val) && item_uid_val->valuestring) {
                            strncpy(auth_data->uid, item_uid_val->valuestring, RTDB_UID_MAX_LEN - 1);
                            strncpy(s_uid, item_uid_val->valuestring, MAX_UID_LEN_INTERNAL - 1);
                            s_uid[MAX_UID_LEN_INTERNAL -1] = '\0'; // Null terminate
                            auth_data->uid[RTDB_UID_MAX_LEN -1] = '\0';
                            has_uid = true;
                        }
                        ESP_LOGI(TAG, "Parsed from response: id_token len=%d, refresh_token len=%d, uid len=%d",
                            strlen(s_id_token), strlen(s_refresh_token), strlen(s_uid));
                        ESP_LOGD(TAG, "Parsed id_token (preview): %.30s...", s_id_token);
                        ESP_LOGD(TAG, "Parsed refresh_token (preview): %.30s...", s_refresh_token);
                        ESP_LOGD(TAG, "Parsed uid: %s", s_uid);
/*
                        // Kiểm tra các trường thiết yếu đã được parse thành công chưa
                                bool uid_check_ok = false;
                                if (strstr(url_buffer, "accounts:signUp") != NULL) { // Anonymous sign-up response
                                    if (strlen(parsed_uid) > 0) {
                                        uid_check_ok = true;
                                    } else {
                                        ESP_LOGE(TAG, "Anonymous sign-up response missing 'localId'.");
                                    }
                                } else if (strstr(url_buffer, "/v1/token") != NULL) { // Token refresh response
                                    if (strlen(parsed_uid) > 0) { // parsed_uid lúc này là từ "user_id"
                                        if (strlen(s_uid) > 0 && strcmp(parsed_uid, s_uid) == 0) {
                                            uid_check_ok = true; // UID từ server khớp với UID đã lưu (s_uid)
                                        } else if (strlen(s_uid) == 0) {
                                            ESP_LOGW(TAG, "Token Refresh: s_uid was empty. Accepting UID from refresh response: %s", parsed_uid);
                                            uid_check_ok = true; // Chấp nhận nếu s_uid rỗng (trường hợp hiếm khi refresh)
                                        } else {
                                            ESP_LOGE(TAG, "Token Refresh UID MISMATCH! Stored s_uid: '%s', received user_id: '%s'. This should not happen.", s_uid, parsed_uid);
                                            // Nếu UID không khớp, đây là vấn đề nghiêm trọng, có thể token không thuộc về user này.
                                            // Bạn có thể quyết định coi đây là parse_fail.
                                        }
                                    } else {
                                        ESP_LOGE(TAG, "Token refresh response missing 'user_id'.");
                                    }
                                } else {
                                    ESP_LOGE(TAG, "Unknown auth URL type for UID check: %s", url_buffer);
                                }

                                */



                        // Set parse_success nếu tất cả các fields cần thiết đều có
                        parse_success = (has_id_token && has_refresh_token && has_uid);

                        if (parse_success) {
                            auth_data->status = ESP_OK;
                            ESP_LOGI(TAG, "Firebase Auth response parsed successfully");
                            ESP_LOGI(TAG, "Auth/Refresh successful. UID: %s. Saving tokens to NVS.", s_uid);
                                nvs_handle_t nvs_handle;
                                esp_err_t nvs_err_save = nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &nvs_handle);
                                if (nvs_err_save == ESP_OK) {
                                    _nvs_write_string(nvs_handle, NVS_KEY_ID_TOKEN, s_id_token);
                                    _nvs_write_string(nvs_handle, NVS_KEY_REFRESH_TOKEN, s_refresh_token);
                                    _nvs_write_string(nvs_handle, NVS_KEY_UID, s_uid);
                                    // s_web_api_key đã nên được lưu khi rtdb_client_authenticate_device được gọi lần đầu
                                    // _nvs_write_string(nvs_h_save, NVS_KEY_API_KEY, s_web_api_key); // Ghi lại nếu muốn
                                    esp_err_t commit_err = nvs_commit(nvs_handle);
                                    if (commit_err != ESP_OK) {
                                        ESP_LOGE(TAG, "Failed to commit NVS after saving tokens: %s", esp_err_to_name(commit_err));
                                    } else {
                                        ESP_LOGI(TAG, "Tokens and UID saved/updated in NVS successfully.");

                                    char id_token_verify[MAX_ID_TOKEN_LEN_INTERNAL] = {0};
                                    char refresh_token_verify[MAX_REFRESH_TOKEN_LEN_INTERNAL] = {0};
                                    char uid_verify[MAX_UID_LEN_INTERNAL] = {0};
                                    _nvs_read_string(nvs_handle, NVS_KEY_ID_TOKEN, id_token_verify, MAX_ID_TOKEN_LEN_INTERNAL);
                                    _nvs_read_string(nvs_handle, NVS_KEY_REFRESH_TOKEN, refresh_token_verify, MAX_REFRESH_TOKEN_LEN_INTERNAL);
                                    _nvs_read_string(nvs_handle, NVS_KEY_UID, uid_verify, MAX_UID_LEN_INTERNAL);

                                    ESP_LOGI(TAG, "NVS Verify - ID Token: %s", id_token_verify);
                                    ESP_LOGI(TAG, "NVS Verify - Refresh Token: %s", refresh_token_verify);
                                    ESP_LOGI(TAG, "NVS Verify - UID: %s", uid_verify);


                                    }
                                    nvs_close(nvs_handle);
                                } else {
                                    ESP_LOGE(TAG, "Failed to open NVS to save tokens: %s", esp_err_to_name(nvs_err_save));
                                }


                        } else {
                            auth_data->status = ESP_FAIL;
                            strncpy(auth_data->error_message, "Incomplete auth data", RTDB_ERROR_MSG_MAX_LEN -1);
                            ESP_LOGW(TAG, "Firebase Auth response parsing incomplete - missing required fields");
                        }
                        
                        cJSON_Delete(json_root);
                    } else { auth_data->status = ESP_FAIL;
                        strncpy(auth_data->error_message, "JSON Parse Fail", RTDB_ERROR_MSG_MAX_LEN-1);
                        ESP_LOGE(TAG, "Auth JSON Parse Fail: %s", cJSON_GetErrorPtr());}
                } else {    // Nếu không phải 200, kiểm tra xem có phải lỗi từ Refresh Token không
                    
                    char url_check_fallback[128];
                    if (evt->client && esp_http_client_get_url(evt->client, url_check_fallback, sizeof(url_check_fallback)) == ESP_OK) {
                        if (strstr(url_check_fallback, "securetoken.googleapis.com/v1/token") != NULL && http_status != 200) { // Đây là lỗi từ Refresh Token
                            ESP_LOGW(TAG, "Token refresh failed (HTTP %d). Clearing NVS tokens and RAM, then attempting new anonymous sign-in.", http_status);
                            //In nội dung response để debug
                            if (user_data_from_event->response_buffer_http) {
                                ESP_LOGE(TAG, "Response: %s", user_data_from_event->response_buffer_http);
                            } else {
                                ESP_LOGE(TAG, "No response buffer available for error details.");
                            }


                            // Xóa tokens khỏi NVS
                            nvs_handle_t nvs_h_fallback;
                            if (nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &nvs_h_fallback) == ESP_OK) {
                                nvs_erase_key(nvs_h_fallback, NVS_KEY_ID_TOKEN); // Có thể không cần nếu bạn không lưu
                                nvs_erase_key(nvs_h_fallback, NVS_KEY_REFRESH_TOKEN);
                                nvs_erase_key(nvs_h_fallback, NVS_KEY_UID);
                                esp_err_t commit_err = nvs_commit(nvs_h_fallback);
                                if(commit_err != ESP_OK) ESP_LOGE(TAG, "NVS commit failed during fallback clear: %s", esp_err_to_name(commit_err));
                                nvs_close(nvs_h_fallback);
                                ESP_LOGI(TAG, "Invalid/Expired tokens cleared from NVS due to refresh failure.");
                            } else {
                                ESP_LOGE(TAG, "Failed to open NVS to clear tokens during fallback.");
                            }

                            // Xóa token trong RAM
                            memset(s_id_token, 0, sizeof(s_id_token));
                            memset(s_refresh_token, 0, sizeof(s_refresh_token));
                            memset(s_uid, 0, sizeof(s_uid));

                            // Giải phóng event hiện tại (nếu có) vì nó là kết quả lỗi của refresh
                            if(acquired_event) {
                                event_pool_release_buffer(acquired_event);
                                acquired_event = NULL; // Quan trọng
                            }

                            esp_err_t anon_err = _start_anonymous_signin(); // Bắt đầu đăng nhập ẩn danh mới
                            if (anon_err != ESP_OK) {
                                ESP_LOGE(TAG, "Failed to start fallback anonymous sign-in: %s", esp_err_to_name(anon_err));
                                // Có thể gửi event lỗi nghiêm trọng ở đây nếu ngay cả việc này cũng thất bại
                            }
                            if (user_data_from_event) {
                                // KHÔNG giải phóng user_data_from_event hoặc response_buffer_http ở đây.
                                // Chỉ reset buffer để chuẩn bị nhận dữ liệu phản hồi.
                                user_data_from_event->current_response_http_len = 0;
                                if (user_data_from_event->response_buffer_http) {
                                    user_data_from_event->response_buffer_http[0] = '\0';
                                }
                            }
                            return ESP_OK; // Đã xử lý xong fallback, thoát khỏi event handler này.
                        }
                    }
                    // Nếu không phải lỗi từ refresh token, thì điền thông tin lỗi vào acquired_event và để nó được gửi đi
                    auth_data->status = ESP_FAIL;
                    snprintf(auth_data->error_message, RTDB_ERROR_MSG_MAX_LEN, "Auth HTTP Error %d", http_status);
                    
                    ESP_LOGE(TAG, "Auth HTTP Error %d. Response: %s", http_status, user_data_from_event->response_buffer_http ? user_data_from_event->response_buffer_http : "N/A");
                }
                auth_data->parse_success = parse_success;   //thêm cờ parse_success
            }
            else if (user_data_from_event->event_type_on_success == EVENT_TYPE_RTDB_GET_RESULT) {
                rtdb_get_result_data_t* get_data_res = &acquired_event->data.rtdb_get_result;
                get_data_res->http_status_code = http_status;
                strncpy(get_data_res->path, user_data_from_event->requested_path_for_get, RTDB_PATH_MAX_LEN -1);

                if (http_status == 200) {
                    get_data_res->status = ESP_OK;
                    if (user_data_from_event->response_buffer_http && user_data_from_event->current_response_http_len > 0) {
                        strncpy(get_data_res->response_preview, user_data_from_event->response_buffer_http, RTDB_RESPONSE_PREVIEW_MAX_LEN - 1);
                        get_data_res->response_preview[RTDB_RESPONSE_PREVIEW_MAX_LEN - 1] = '\0';
                    } else { get_data_res->response_preview[0] = '\0'; }
                } else {
                    get_data_res->status = ESP_FAIL;
                    if (user_data_from_event->response_buffer_http) strncpy(get_data_res->response_preview, user_data_from_event->response_buffer_http, RTDB_RESPONSE_PREVIEW_MAX_LEN -1); // Copy error response
                    else get_data_res->response_preview[0] = '\0';
                }
            }
            // TODO: Tương tự cho PUT_RESULT, PATCH_RESULT, DELETE_RESULT

            if (g_master_event_queue_ptr != NULL) {
                if (xQueueSend(g_master_event_queue_ptr, &acquired_event, pdMS_TO_TICKS(100)) != pdPASS) {
                    ESP_LOGE(TAG, "Failed to send event (type %d) to master queue! Releasing buffer.", acquired_event->type);
                    event_pool_release_buffer(acquired_event); // Quan trọng: Trả lại buffer nếu không gửi được
                } else {
                    // Event đã được gửi thành công, con trỏ acquired_event giờ thuộc về queue/task nhận
                }
            } else {
                ESP_LOGE(TAG, "Master event queue is NULL! Releasing acquired buffer.");
                event_pool_release_buffer(acquired_event);
            }
            break;
        // (Giữ nguyên các case khác)
        case HTTP_EVENT_DISCONNECTED: ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED"); break;
        case HTTP_EVENT_REDIRECT: ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        //esp_http_client_set_header(evt->client, "Host", "");
        esp_http_client_set_redirection(evt->client); break;
        default: ESP_LOGD(TAG, "Unhandled HTTP Event: %d", evt->event_id); break;
    }
    return ESP_OK;
}

// Trong file: rtdb_client.c

static void http_request_task(void *pvParameters) {
    esp_http_client_config_t *config = (esp_http_client_config_t *)pvParameters;
    http_event_handler_user_data_t *handler_user_data = NULL;
    char temp_url_for_log[256] = "URL_UNKNOWN"; // Tăng kích thước buffer cho URL log

    // --- Kiểm tra đầu vào ---
    if (!config) {
        ESP_LOGE(TAG, "http_request_task: Received NULL config pointer. Task exiting.");
        vTaskDelete(NULL);
        return;
    }

    if (config->url) {
        strncpy(temp_url_for_log, config->url, sizeof(temp_url_for_log) - 1);
        temp_url_for_log[sizeof(temp_url_for_log) - 1] = '\0'; // Đảm bảo null-terminated
    }

    handler_user_data = (http_event_handler_user_data_t *)config->user_data;

    if (!handler_user_data) {
        ESP_LOGE(TAG, "http_request_task: handler_user_data is NULL for URL: %s. Task exiting.", temp_url_for_log);
        // Dọn dẹp config nếu đã được cấp phát URL
        if(config->url) free((void*)config->url);
        free(config);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTP Task: Performing request to %s", temp_url_for_log);
    esp_http_client_handle_t client = esp_http_client_init(config);

    // --- Xử lý lỗi khi khởi tạo HTTP client ---
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP client for URL: %s", temp_url_for_log);
        system_event_t* err_event_buffer = event_pool_acquire_buffer(0);
        if (err_event_buffer) {
            err_event_buffer->type = handler_user_data->event_type_on_success;
            err_event_buffer->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
            // Điền thông tin lỗi cụ thể dựa trên loại event
            if (handler_user_data->event_type_on_success == EVENT_TYPE_HTTP_TEST_RESULT) {
                err_event_buffer->data.http_test_result.status = ESP_FAIL;
                err_event_buffer->data.http_test_result.http_status_code = -100; // Mã lỗi client init
            } else if (handler_user_data->event_type_on_success == EVENT_TYPE_FIREBASE_AUTH_RESULT) {
                err_event_buffer->data.firebase_auth_result.status = ESP_FAIL;
                err_event_buffer->data.firebase_auth_result.http_status_code = -100;
                strncpy(err_event_buffer->data.firebase_auth_result.error_message, "HTTP Client Init Fail", RTDB_ERROR_MSG_MAX_LEN-1);
                err_event_buffer->data.firebase_auth_result.error_message[RTDB_ERROR_MSG_MAX_LEN-1] = '\0';
            } else if (handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_GET_RESULT) {
                err_event_buffer->data.rtdb_get_result.status = ESP_FAIL;
                err_event_buffer->data.rtdb_get_result.http_status_code = -100;
                strncpy(err_event_buffer->data.rtdb_get_result.path, handler_user_data->requested_path_for_get, RTDB_PATH_MAX_LEN-1);
                err_event_buffer->data.rtdb_get_result.path[RTDB_PATH_MAX_LEN-1] = '\0';
                strncpy(err_event_buffer->data.rtdb_get_result.response_preview, "Client Init Fail", RTDB_RESPONSE_PREVIEW_MAX_LEN -1);
                err_event_buffer->data.rtdb_get_result.response_preview[RTDB_RESPONSE_PREVIEW_MAX_LEN -1] = '\0';
            }
            // Thêm các else if cho EVENT_TYPE_RTDB_PUT_RESULT, PATCH_RESULT, DELETE_RESULT, QUERY_RESULT nếu cần
            // ví dụ:
            // else if (handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_PUT_RESULT) {
            //     err_event_buffer->data.rtdb_write_result.status = ESP_FAIL;
            //     err_event_buffer->data.rtdb_write_result.http_status_code = -100;
            //     strncpy(err_event_buffer->data.rtdb_write_result.path, handler_user_data->requested_path_for_get, RTDB_PATH_MAX_LEN-1);
            //     err_event_buffer->data.rtdb_write_result.path[RTDB_PATH_MAX_LEN-1] = '\0';
            // }

            if (g_master_event_queue_ptr != NULL) {
                 if (xQueueSend(g_master_event_queue_ptr, &err_event_buffer, 0) != pdPASS) {
                      ESP_LOGE(TAG, "Failed to send Client Init Fail event, releasing buffer.");
                      event_pool_release_buffer(err_event_buffer);
                 }
            } else {
                 ESP_LOGE(TAG, "Master queue NULL, releasing Client Init Fail event buffer.");
                 event_pool_release_buffer(err_event_buffer);
            }
        } else {
            ESP_LOGE(TAG, "Failed to acquire buffer for Client Init Fail event.");
        }

        // Dọn dẹp bộ nhớ đã cấp phát trước đó
        if(config->url) free((void*)config->url);
        if(handler_user_data && handler_user_data->response_buffer_http) free(handler_user_data->response_buffer_http);
        if(handler_user_data) free(handler_user_data);
        if(config) free(config);
        vTaskDelete(NULL);
        return;
    }

    // --- Thiết lập Headers và POST data ---

    // 1. POST cho Firebase Anonymous Auth (payload đã được hardcode)
/*    if (config->method == HTTP_METHOD_POST && handler_user_data->event_type_on_success == EVENT_TYPE_FIREBASE_AUTH_RESULT) {
        const char *post_data_auth = "{\"returnSecureToken\":true}";
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data_auth, strlen(post_data_auth));
        ESP_LOGD(TAG, "Set headers for Firebase Auth.");
    }
    // 2. PUT/PATCH payload cho RTDB
    // (payload đã được chuẩn bị trong handler_user_data->response_buffer_http từ rtdb_client_create_http_task)
    else if ((config->method == HTTP_METHOD_PUT || config->method == HTTP_METHOD_PATCH) &&
             (handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_PUT_RESULT ||
              handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_PATCH_RESULT)) {
        if (handler_user_data->response_buffer_http && handler_user_data->current_response_http_len > 0) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, handler_user_data->response_buffer_http, handler_user_data->current_response_http_len);
            ESP_LOGD(TAG, "Set Content-Type and POST field for PUT/PATCH. Length: %d", handler_user_data->current_response_http_len);
        } else {
            ESP_LOGW(TAG, "No payload data provided for PUT/PATCH request to %s despite matching event type.", temp_url_for_log);
        }
    }
*/
// --- Thiết lập POST/PUT/PATCH data ---
if ((config->method == HTTP_METHOD_POST || config->method == HTTP_METHOD_PUT || config->method == HTTP_METHOD_PATCH)) {
    // Ưu tiên 1: Xử lý POST cho Anonymous Sign-Up (nếu không có payload nào được chuẩn bị)
    if (config->method == HTTP_METHOD_POST &&
        handler_user_data->event_type_on_success == EVENT_TYPE_FIREBASE_AUTH_RESULT &&
        strstr(config->url, "accounts:signUp") != NULL && // URL của anonymous sign-up
        handler_user_data->current_response_http_len == 0) { // Không có payload từ create_task

        const char *post_data_anon = "{\"returnSecureToken\":true}";
        ESP_LOGI(TAG, "http_request_task: Setting hardcoded payload for ANONYMOUS SIGN-UP: %s", post_data_anon);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data_anon, strlen(post_data_anon));

    // Ưu tiên 2: Xử lý POST cho Token Refresh (nếu không có payload nào được chuẩn bị)
    } else if (config->method == HTTP_METHOD_POST &&
               handler_user_data->event_type_on_success == EVENT_TYPE_FIREBASE_AUTH_RESULT &&
               strstr(config->url, "securetoken.googleapis.com/v1/token") != NULL && // URL của refresh token
               handler_user_data->current_response_http_len == 0) { // Không có payload từ create_task

        if (strlen(s_refresh_token) > 0) {
            char refresh_payload_task[4096]; // Buffer đủ lớn cho refresh token payload
            snprintf(refresh_payload_task, sizeof(refresh_payload_task),
                     "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}",
                     s_refresh_token); // Sử dụng s_refresh_token đã load từ NVS

            ESP_LOGI(TAG, "http_request_task: Setting hardcoded payload for TOKEN REFRESH: %s", refresh_payload_task);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, refresh_payload_task, strlen(refresh_payload_task));
        } else {
            ESP_LOGE(TAG, "http_request_task: Attempting token refresh, but s_refresh_token is empty! Cannot create payload.");
            // Yêu cầu này có thể sẽ thất bại, hoặc gửi body rỗng.
            // Cần đảm bảo s_refresh_token được load đúng.
        }

    // Ưu tiên 3: Sử dụng payload đã chuẩn bị (cho PUT, PATCH, hoặc POST nếu có)
    } else if (handler_user_data->response_buffer_http &&
               handler_user_data->current_response_http_len > 0) {
        // Áp dụng cho PUT, PATCH, hoặc các trường hợp POST mà payload đã được chuẩn bị trong response_buffer_http
        esp_http_client_set_header(client, "Content-Type", "application/json");
        ESP_LOGI(TAG, "http_request_task: Using PREPARED payload from user_data. Method: %d, Len: %d, URL: %s",
                 config->method,
                 handler_user_data->current_response_http_len,
                 temp_url_for_log);
        ESP_LOG_BUFFER_HEXDUMP(TAG, handler_user_data->response_buffer_http,
                               (handler_user_data->current_response_http_len > 64 ? 64 : handler_user_data->current_response_http_len),
                               ESP_LOG_INFO);
        ESP_LOGI(TAG, "http_request_task: Prepared Payload String (Actual): %s",
                 (handler_user_data->response_buffer_http ? handler_user_data->response_buffer_http : "NULL_BUFFER"));

        esp_http_client_set_post_field(client,
                                       handler_user_data->response_buffer_http,
                                       handler_user_data->current_response_http_len);
    } else {
        // Không có payload nào được chuẩn bị hoặc áp dụng
        ESP_LOGW(TAG, "http_request_task: Method is POST/PUT/PATCH for URL [%s] but no applicable payload was set. Sending request without body (if client allows).",
                 temp_url_for_log);
    }
}
    // 3. Thiết lập Authorization: Bearer header CHỈ KHI auth KHÔNG CÓ TRONG URL (auth_in_url == false)
    //    và đây là các yêu cầu RTDB (không phải Firebase Auth, không phải HTTP Test chung).
    if (!handler_user_data->auth_in_url &&
        (config->method == HTTP_METHOD_GET || config->method == HTTP_METHOD_PUT || config->method == HTTP_METHOD_PATCH || config->method == HTTP_METHOD_DELETE) &&
        (handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_GET_RESULT ||
         handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_PUT_RESULT ||
         handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_PATCH_RESULT ||
         handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_DELETE_RESULT ||
         handler_user_data->event_type_on_success == EVENT_TYPE_RTDB_QUERY_RESULT) ) {
        if (strlen(s_id_token) > 0) {
            // Kích thước buffer cho "Bearer " (7) + token + null terminator (1)
            char auth_header_val[7 + MAX_ID_TOKEN_LEN_INTERNAL + 1];
            snprintf(auth_header_val, sizeof(auth_header_val), "Bearer %s", s_id_token);
            esp_http_client_set_header(client, "Authorization", auth_header_val);
            ESP_LOGD(TAG, "Authorization: Bearer header set for RTDB request to %s", temp_url_for_log);
        } else {
            ESP_LOGW(TAG, "No ID token available for RTDB request to %s (Authorization header attempt, auth_in_url is false)", temp_url_for_log);
            // Yêu cầu này có thể sẽ thất bại với lỗi 401 từ server.
            // Cân nhắc có nên hủy yêu cầu hoặc gửi event lỗi ngay tại đây không.
        }
    } else if (handler_user_data->auth_in_url) {
        ESP_LOGD(TAG, "Skipping Authorization: Bearer header setup for %s because auth_in_url is true.", temp_url_for_log);
    }


    // --- Thực hiện yêu cầu HTTP ---
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request perform failed for %s: %s", temp_url_for_log, esp_err_to_name(err));
        // Lưu ý: Event lỗi thường được gửi từ _http_event_handler_cb qua HTTP_EVENT_ERROR.
        // Không cần gửi event lỗi trùng lặp ở đây trừ khi HTTP_EVENT_ERROR không được kích hoạt.
    }

    // --- Dọn dẹp client ---
    esp_http_client_cleanup(client);

    // --- Dọn dẹp bộ nhớ đã cấp phát cho config và handler_user_data ---
    // config->url được strdup trong rtdb_client_create_http_task, nên cần free ở đây.
    if(config->url) free((void*)config->url);
    // handler_user_data và handler_user_data->response_buffer_http cũng được cấp phát trong rtdb_client_create_http_task
    if(handler_user_data) {
        if(handler_user_data->response_buffer_http) free(handler_user_data->response_buffer_http);
        free(handler_user_data);
    }
    // Cuối cùng là free chính struct config
    if(config) free(config);

    ESP_LOGI(TAG, "HTTP Task for %s finished and resources cleaned up.", temp_url_for_log);
    vTaskDelete(NULL); // Xóa task hiện tại
}

esp_err_t rtdb_client_init(QueueHandle_t main_event_queue_ptr_param) {
    if (main_event_queue_ptr_param == NULL) { ESP_LOGE(TAG, "Master event queue handle is NULL!"); return ESP_ERR_INVALID_ARG; }
    g_master_event_queue_ptr = main_event_queue_ptr_param;
    memset(s_id_token, 0, sizeof(s_id_token));
    memset(s_refresh_token, 0, sizeof(s_refresh_token));
    ESP_LOGI(TAG, "RTDB Client Initialized.");
    return ESP_OK;
}


// Hàm authenticate_device được gọi từ bên ngoài để bắt đầu quá trình xác thực
esp_err_t rtdb_client_authenticate_device(const char* web_api_key_param) {
    ESP_LOGI(TAG, "rtdb_client_authenticate_device called.");
    nvs_handle_t nvs_h_apikey;
    esp_err_t nvs_err_apikey;
    bool api_key_set = false;
    // 1. Kiểm tra và xử lý API Key
    // Nếu web_api_key_param không rỗng, sử dụng nó và lưu vào NVS
    // Nếu không, cố gắng tải từ NVS
if (web_api_key_param && strlen(web_api_key_param) > 0) {
        if (strlen(web_api_key_param) < MAX_API_KEY_LEN_INTERNAL) {
            strncpy(s_web_api_key, web_api_key_param, MAX_API_KEY_LEN_INTERNAL - 1);
            s_web_api_key[MAX_API_KEY_LEN_INTERNAL - 1] = '\0';
            ESP_LOGI(TAG, "Using API key from parameter for auth process. Will save to NVS.");
            api_key_set = true;
            // Lưu vào NVS
            nvs_err_apikey = nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &nvs_h_apikey);
            if (nvs_err_apikey == ESP_OK) {
                _nvs_write_string(nvs_h_apikey, NVS_KEY_API_KEY, s_web_api_key);
                nvs_commit(nvs_h_apikey);
                nvs_close(nvs_h_apikey);
            } else {
                ESP_LOGE(TAG, "Failed to open NVS to save API key from param for auth task (%s)", esp_err_to_name(nvs_err_apikey));
            }
        } else {
            ESP_LOGE(TAG, "Web API Key parameter too long for auth process.");
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        ESP_LOGI(TAG, "No API key from param for auth process, trying to load from NVS.");
        nvs_err_apikey = nvs_open(NVS_NAMESPACE_AUTH, NVS_READONLY, &nvs_h_apikey);
        if (nvs_err_apikey == ESP_OK) {
            _nvs_read_string(nvs_h_apikey, NVS_KEY_API_KEY, s_web_api_key, MAX_API_KEY_LEN_INTERNAL);
            nvs_close(nvs_h_apikey);
            if (strlen(s_web_api_key) > 0) {
                ESP_LOGI(TAG, "API key loaded from NVS for auth process.");
                api_key_set = true;
            } else {
                ESP_LOGW(TAG, "No API key found in NVS for auth process.");
            }
        } else {
            ESP_LOGE(TAG, "Failed to open NVS to read API key for auth process (%s)", esp_err_to_name(nvs_err_apikey));
        }
    }

    if (!api_key_set) {
        ESP_LOGE(TAG, "Web API Key is NOT available. Cannot start authentication task.");
        return ESP_ERR_INVALID_STATE;
    }
    // --- Kết thúc Logic xử lý API Key ---

    // 2. Tạo task để thực hiện xác thực
    // if (rtdb_auth_task_handle != NULL) {
    //     ESP_LOGW(TAG, "Authentication task may still be running. Not creating a new one.");
    //     return ESP_ERR_INVALID_STATE; // Hoặc một cơ chế khác để xử lý việc gọi lại
    // }

    BaseType_t task_created = xTaskCreate(
        rtdb_authentication_worker_task,
        "AuthWorkerTask",       // Tên task
        10240,                   // Kích thước stack (điều chỉnh nếu cần)
        NULL,                   // Tham số task (không cần vì dùng biến static)
        6,                      // Độ ưu tiên
        NULL // &rtdb_auth_task_handle // Handle task (tùy chọn)
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create rtdb_authentication_worker_task.");
        // rtdb_auth_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "rtdb_authentication_worker_task created. It will handle auth logic.");
    return ESP_OK; // Trả về OK nếu task được tạo thành công. Kết quả xác thực sẽ qua event.
}

// Hàm chung để tạo HTTP request task
static esp_err_t rtdb_client_create_http_task(const char* url_str, esp_http_client_method_t method,
                                             system_event_type_t success_event_type,
                                             const char* original_request_payload, // Đổi tên thành original_request_payload để rõ ràng hơn
                                             const char* requested_path)
{
    ESP_LOGI(TAG, "--- rtdb_client_create_http_task ---"); // Log bắt đầu hàm
    ESP_LOGI(TAG, "URL: %s, Method: %d, EventType: %d, ReqPath: %s",
             url_str ? url_str : "NULL_URL",
             method, success_event_type, requested_path ? requested_path : "NULL_PATH");

    ESP_LOGI(TAG, "Received original_request_payload_ptr: %p", original_request_payload);
    if (original_request_payload) {
        ESP_LOGI(TAG, "strlen(original_request_payload) at entry: %d", strlen(original_request_payload));
        ESP_LOGI(TAG, "Original request payload: %20s",
                 original_request_payload ? original_request_payload : "NULL_PAYLOAD");
    } else {
        ESP_LOGI(TAG, "Received original_request_payload is NULL");
    }
    
    if (!g_master_event_queue_ptr) {
        ESP_LOGE(TAG, "RTDB Client not initialized (master event queue is NULL).");
        return ESP_ERR_INVALID_STATE;
    }
    if (!url_str || strlen(url_str) == 0) {
        ESP_LOGE(TAG, "URL is invalid (NULL or empty).");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t *config = calloc(1, sizeof(esp_http_client_config_t));
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTTP client config.");
        return ESP_ERR_NO_MEM;
    }


    http_event_handler_user_data_t *handler_user_data = calloc(1, sizeof(http_event_handler_user_data_t));
    if (!handler_user_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTTP handler user data.");
        free(config);
        return ESP_ERR_NO_MEM;
    }

    // Xác định kích thước buffer response dựa trên loại yêu cầu
    if (success_event_type == EVENT_TYPE_HTTP_TEST_RESULT ||
        success_event_type == EVENT_TYPE_RTDB_GET_RESULT ||
        success_event_type == EVENT_TYPE_RTDB_QUERY_RESULT) {
        handler_user_data->response_buffer_http_size = 4096;
    } else if (success_event_type == EVENT_TYPE_FIREBASE_AUTH_RESULT) {
        handler_user_data->response_buffer_http_size = 8192; // 8KB cho Firebase Auth, đủ cho token và thông tin người dùng
    } else { // PUT, PATCH, DELETE result
        handler_user_data->response_buffer_http_size = 2048;
    }
    ESP_LOGI(TAG, "Allocating response_buffer_http_size: %d for event_type: %d",
             handler_user_data->response_buffer_http_size, success_event_type);

    handler_user_data->response_buffer_http = malloc(handler_user_data->response_buffer_http_size);
    
    if (!handler_user_data->response_buffer_http) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTTP response buffer.");
        free(handler_user_data);
        free(config);
        return ESP_ERR_NO_MEM;
    }
    // Khởi tạo buffer response_buffer_http để đảm bảo nó là chuỗi rỗng ban đầu
    handler_user_data->response_buffer_http[0] = '\0';
    handler_user_data->current_response_http_len = 0; // Quan trọng: dùng để đếm độ dài response nhận về

    handler_user_data->event_type_on_success = success_event_type;
    if (requested_path) {
        strncpy(handler_user_data->requested_path_for_get, requested_path, RTDB_PATH_MAX_LEN - 1);
        handler_user_data->requested_path_for_get[RTDB_PATH_MAX_LEN - 1] = '\0';
    } else {
        handler_user_data->requested_path_for_get[0] = '\0';
    }

    if (strstr(url_str, "?auth=") != NULL) {
        handler_user_data->auth_in_url = true;
        ESP_LOGD(TAG, "Auth token detected in URL. auth_in_url = true.");
    } else {
        handler_user_data->auth_in_url = false;
        ESP_LOGD(TAG, "Auth token NOT in URL. auth_in_url = false.");
    }
    // Xử lý payload cho các method yêu cầu body (POST, PUT, PATCH)
    // current_response_http_len sẽ lưu độ dài của REQUEST PAYLOAD TẠM THỜI
    // response_buffer_http sẽ chứa REQUEST PAYLOAD TẠM THỜI
    switch (method) {
        case HTTP_METHOD_POST:
            ESP_LOGI(TAG, "Processing HTTP_METHOD_POST.");
            if (original_request_payload) {
                size_t payload_len = strlen(original_request_payload);
                ESP_LOGI(TAG, "Original POST payload_len: %d", payload_len);
                if (payload_len == 0) {
                    ESP_LOGW(TAG, "Original POST payload is an empty string.");
                    handler_user_data->current_response_http_len = 0;
                    handler_user_data->response_buffer_http[0] = '\0';
                } else if (payload_len < handler_user_data->response_buffer_http_size) {
                    memcpy(handler_user_data->response_buffer_http, original_request_payload, payload_len);
                    handler_user_data->response_buffer_http[payload_len] = '\0'; // Null-terminate
                    handler_user_data->current_response_http_len = payload_len; // Đây là độ dài của request payload
                    ESP_LOGI(TAG, "POST payload copied to response_buffer_http. Length: %d", handler_user_data->current_response_http_len);
                    ESP_LOGI(TAG, "POST payload (first 64 bytes): %.64s",
                             handler_user_data->response_buffer_http ? handler_user_data->response_buffer_http : "NULL_BUFFER");
                    
                } else {
                    ESP_LOGE(TAG, "POST payload (len: %zu) too large for response_buffer_http (size: %d).",
                             payload_len, handler_user_data->response_buffer_http_size);
                    goto cleanup_and_fail_mem;
                }
            } else {
                ESP_LOGW(TAG, "HTTP_METHOD_POST but original_request_payload is NULL. current_response_http_len set to 0.");
                handler_user_data->current_response_http_len = 0;
                handler_user_data->response_buffer_http[0] = '\0';
            }
            break;

        case HTTP_METHOD_PUT:
        case HTTP_METHOD_PATCH:
            ESP_LOGI(TAG, "Processing HTTP_METHOD_PUT/PATCH.");
            if (original_request_payload) {
                size_t payload_len = strlen(original_request_payload);
                 ESP_LOGI(TAG, "Original PUT/PATCH payload_len: %d", payload_len);
                if (payload_len == 0) {
                    ESP_LOGW(TAG, "Original PUT/PATCH payload is an empty string.");
                    handler_user_data->current_response_http_len = 0;
                    handler_user_data->response_buffer_http[0] = '\0';
                } else if (payload_len < handler_user_data->response_buffer_http_size) {
                    // Lưu ý: response_buffer_http đang được dùng để chứa request payload
                    memcpy(handler_user_data->response_buffer_http, original_request_payload, payload_len);
                    handler_user_data->response_buffer_http[payload_len] = '\0';
                    handler_user_data->current_response_http_len = payload_len;
                    ESP_LOGI(TAG, "PUT/PATCH payload copied to response_buffer_http. Length: %d", handler_user_data->current_response_http_len);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, handler_user_data->response_buffer_http,
                                           (payload_len > 32 ? 32 : payload_len), ESP_LOG_INFO);
                } else {
                    ESP_LOGE(TAG, "PUT/PATCH payload (len: %zu) too large for response_buffer_http (size: %d).",
                             payload_len, handler_user_data->response_buffer_http_size);
                    goto cleanup_and_fail_mem;
                }
            } else {
                ESP_LOGW(TAG, "HTTP_METHOD_PUT/PATCH but original_request_payload is NULL. current_response_http_len set to 0.");
                handler_user_data->current_response_http_len = 0;
                handler_user_data->response_buffer_http[0] = '\0';
            }
            break;

        case HTTP_METHOD_GET:
        case HTTP_METHOD_DELETE:
        case HTTP_METHOD_HEAD: // Thêm các method không có body nếu cần
        default:
            // Đối với các method không có body, current_response_http_len dùng để nhận response
            handler_user_data->current_response_http_len = 0;
            handler_user_data->response_buffer_http[0] = '\0';
            ESP_LOGI(TAG, "Method is GET/DELETE/HEAD or unhandled. current_response_http_len set to 0 for receiving response.");
            break;
    }

    config->url = strdup(url_str);
    if (!config->url) {
        ESP_LOGE(TAG, "Failed to duplicate URL string.");
        goto cleanup_and_fail_mem;
    }

    config->event_handler = _http_event_handler_cb;
    config->user_data = handler_user_data;
    config->method = method;
    config->crt_bundle_attach = esp_crt_bundle_attach;
    config->timeout_ms = 15000;
    config->buffer_size = 8192;
    config->buffer_size_tx = 2048;

    char task_name[32];
    snprintf(task_name, sizeof(task_name), "http_req_%lu", (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF));

    // Log debug TRƯỚC KHI TẠO TASK HTTP
    ESP_LOGI(TAG, "--- Preparing to create HTTP task ---");
    ESP_LOGI(TAG, "Task Name: %s, URL: %s", task_name, config->url);
    ESP_LOGI(TAG, "Method: %d", config->method);
    ESP_LOGI(TAG, "Handler User Data: EventType: %d, AuthInURL: %s, ReqPath: %s",
             handler_user_data->event_type_on_success,
             handler_user_data->auth_in_url ? "true" : "false",
             handler_user_data->requested_path_for_get);
    ESP_LOGI(TAG, "Handler User Data: ResponseBufferSize: %d", handler_user_data->response_buffer_http_size);
    ESP_LOGI(TAG, "Handler User Data: CurrentResponseHttpLen (REQUEST PAYLOAD LENGTH for POST/PUT/PATCH): %d",
             handler_user_data->current_response_http_len);
    if (handler_user_data->current_response_http_len > 0) {
         ESP_LOGI(TAG, "Handler User Data: ResponseBufferHttp (REQUEST PAYLOAD for POST/PUT/PATCH - first 64): %.64s",
                  handler_user_data->response_buffer_http);
    } else {
         ESP_LOGI(TAG, "Handler User Data: ResponseBufferHttp (REQUEST PAYLOAD for POST/PUT/PATCH): EMPTY or N/A");
    }
    // Kết thúc log debug

    if (xTaskCreate(http_request_task, task_name, 10240, config, 5, NULL) != pdPASS) { // Giữ stack 10240
        ESP_LOGE(TAG, "Failed to create HTTP task for URL: %s", config->url);
        if(config->url) free((void*)config->url); // Giải phóng URL đã strdup
        goto cleanup_and_fail_mem;
    }
    ESP_LOGI(TAG, "HTTP task '%s' created successfully.", task_name);
    return ESP_OK;

cleanup_and_fail_mem:
    if (handler_user_data) {
        if (handler_user_data->response_buffer_http) free(handler_user_data->response_buffer_http);
        free(handler_user_data);
    }
    if (config) {
        // config->url có thể chưa được gán nếu lỗi sớm, hoặc đã được strdup
        // Nếu đã strdup mà task tạo lỗi, cần free ở trên.
        // Nếu chưa strdup thì không cần free config->url.
        free(config);
    }
    return ESP_ERR_NO_MEM;
}


esp_err_t rtdb_client_perform_http_test(const char* url_to_test) {
    return rtdb_client_create_http_task(url_to_test, HTTP_METHOD_GET, EVENT_TYPE_HTTP_TEST_RESULT, NULL, NULL);
}

esp_err_t rtdb_client_firebase_anonymous_auth(const char* web_api_key) {
    if (!web_api_key || strlen(web_api_key) == 0) { ESP_LOGE(TAG, "Web API Key is required."); return ESP_ERR_INVALID_ARG; }
    char auth_url[256];
    snprintf(auth_url, sizeof(auth_url), "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=%s", web_api_key);
    // post_payload cho anonymous auth đã được hardcode trong http_request_task, sẽ sửa sau
    return rtdb_client_create_http_task(auth_url, HTTP_METHOD_POST, EVENT_TYPE_FIREBASE_AUTH_RESULT, NULL, NULL);
}

const char* rtdb_client_get_id_token(void) {
    return (strlen(s_id_token) > 0) ? s_id_token : NULL;
}

esp_err_t rtdb_client_get_data(const char* rtdb_base_url, const char* path) {
    const char* id_token = rtdb_client_get_id_token();

    if (!rtdb_base_url || strlen(rtdb_base_url) == 0 || !path || strlen(path) == 0) {
        ESP_LOGE(TAG, "Base URL or Path is NULL/empty for GET.");
        return ESP_ERR_INVALID_ARG;
    }

    if (!id_token || strlen(id_token) == 0) {
        ESP_LOGE(TAG, "No ID Token available for RTDB GET request to path: %s. Please authenticate first.", path);
        system_event_t* err_event = event_pool_acquire_buffer(0);
        if (err_event) {
            err_event->type = EVENT_TYPE_RTDB_GET_RESULT;
            err_event->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
            err_event->data.rtdb_get_result.status = ESP_FAIL;
            err_event->data.rtdb_get_result.http_status_code = -1; // Lỗi nội bộ client (ví dụ: thiếu token)
            strncpy(err_event->data.rtdb_get_result.path, path, RTDB_PATH_MAX_LEN - 1);
            err_event->data.rtdb_get_result.path[RTDB_PATH_MAX_LEN - 1] = '\0';
            strncpy(err_event->data.rtdb_get_result.response_preview, "Error: Client not authenticated (No ID Token)", RTDB_RESPONSE_PREVIEW_MAX_LEN - 1);
            err_event->data.rtdb_get_result.response_preview[RTDB_RESPONSE_PREVIEW_MAX_LEN - 1] = '\0';

            if (g_master_event_queue_ptr != NULL) {
                if (xQueueSend(g_master_event_queue_ptr, &err_event, 0) != pdPASS) {
                    ESP_LOGE(TAG, "Failed to send NO_TOKEN error event for GET to master queue, releasing buffer.");
                    event_pool_release_buffer(err_event);
                }
            } else {
                ESP_LOGE(TAG, "Master event queue is NULL! Releasing NO_TOKEN error event buffer for GET.");
                event_pool_release_buffer(err_event);
            }
        } else {
            ESP_LOGE(TAG, "Failed to acquire event buffer for NO_TOKEN error (GET).");
        }
        return ESP_ERR_INVALID_STATE;
    }

    // Tính toán kích thước buffer cần thiết cho URL đầy đủ
    size_t base_len = strlen(rtdb_base_url);
    size_t path_len = strlen(path);
    size_t token_len = strlen(id_token);
    // URL = base_url + path + ".json?auth=" (11) + id_token + null (1)
    size_t url_buffer_size = base_len + path_len + 11 + token_len + 1;
    char *get_url = malloc(url_buffer_size);
    if (!get_url) {
        ESP_LOGE(TAG, "Failed to allocate memory for GET URL (size: %zu).", url_buffer_size);
        return ESP_ERR_NO_MEM;
    }

    snprintf(get_url, url_buffer_size, "%s%s.json?auth=%s", rtdb_base_url, path, id_token);
    ESP_LOGI(TAG, "Constructed RTDB GET URL with auth token: %s", get_url);

    esp_err_t task_creation_result = rtdb_client_create_http_task(get_url, HTTP_METHOD_GET, EVENT_TYPE_RTDB_GET_RESULT, NULL, path);

    free(get_url); // Giải phóng bộ nhớ đã cấp phát cho URL

    return task_creation_result;
}

esp_err_t rtdb_client_put_data(const char* rtdb_base_url, const char* path, const char* json_payload) {
    const char* id_token = rtdb_client_get_id_token();

    if (!rtdb_base_url || strlen(rtdb_base_url) == 0 ||
        !path || strlen(path) == 0 ||
        !json_payload || strlen(json_payload) == 0) {
        ESP_LOGE(TAG, "Base URL, Path, or JSON payload is NULL/empty for PUT.");
        return ESP_ERR_INVALID_ARG;
    }

    if (!id_token || strlen(id_token) == 0) {
        ESP_LOGE(TAG, "No ID Token available for RTDB PUT request to path: %s.", path);
        // Gửi event lỗi tương tự như trong rtdb_client_get_data nếu cần
        return ESP_ERR_INVALID_STATE;
    }

    size_t base_len = strlen(rtdb_base_url);
    size_t path_len = strlen(path);
    size_t token_len = strlen(id_token);
    size_t url_buffer_size = base_len + path_len + 11 + token_len + 1;
    char *put_url = malloc(url_buffer_size);
    if (!put_url) {
        ESP_LOGE(TAG, "Failed to allocate memory for PUT URL (size: %zu).", url_buffer_size);
        return ESP_ERR_NO_MEM;
    }

    snprintf(put_url, url_buffer_size, "%s%s.json?auth=%s", rtdb_base_url, path, id_token);
    ESP_LOGI(TAG, "Constructed RTDB PUT URL with auth token: %s", put_url);
    ESP_LOGD(TAG, "PUT Payload: %s", json_payload);


    esp_err_t task_creation_result = rtdb_client_create_http_task(put_url, HTTP_METHOD_PUT, EVENT_TYPE_RTDB_PUT_RESULT, json_payload, path);

    free(put_url);

    return task_creation_result;
}

esp_err_t rtdb_client_patch_data(const char* rtdb_base_url, const char* path, const char* json_payload) {
    const char* id_token = rtdb_client_get_id_token();
    if (!rtdb_base_url || !path || !json_payload) { ESP_LOGE(TAG, "Params missing for PATCH."); return ESP_ERR_INVALID_ARG; }
    if (!id_token) { ESP_LOGE(TAG, "No ID Token for RTDB PATCH."); return ESP_ERR_INVALID_STATE;}

    char patch_url[RTDB_PATH_MAX_LEN + 256];
    snprintf(patch_url, sizeof(patch_url), "%s%s.json", rtdb_base_url, path);
    return rtdb_client_create_http_task(patch_url, HTTP_METHOD_PATCH, EVENT_TYPE_RTDB_PATCH_RESULT, json_payload, path);
}

esp_err_t rtdb_client_delete_data(const char* rtdb_base_url, const char* path) {
    const char* id_token = rtdb_client_get_id_token();
    if (!rtdb_base_url || !path) { ESP_LOGE(TAG, "Params missing for DELETE."); return ESP_ERR_INVALID_ARG; }
    if (!id_token) { ESP_LOGE(TAG, "No ID Token for RTDB DELETE."); return ESP_ERR_INVALID_STATE;}

    char delete_url[RTDB_PATH_MAX_LEN + 256];
    snprintf(delete_url, sizeof(delete_url), "%s%s.json", rtdb_base_url, path);
    return rtdb_client_create_http_task(delete_url, HTTP_METHOD_DELETE, EVENT_TYPE_RTDB_DELETE_RESULT, NULL, path);
}

esp_err_t rtdb_client_query_data(const char* rtdb_base_url, const char* path, const char* query_params) {
    const char* id_token = rtdb_client_get_id_token();
    if (!rtdb_base_url || !path || !query_params) { ESP_LOGE(TAG, "Params missing for QUERY."); return ESP_ERR_INVALID_ARG; }
    if (!id_token) { ESP_LOGE(TAG, "No ID Token for RTDB QUERY."); return ESP_ERR_INVALID_STATE;}

    char query_url[RTDB_PATH_MAX_LEN + 256 + 256]; // Thêm chỗ cho query_params
    snprintf(query_url, sizeof(query_url), "%s%s.json?%s", rtdb_base_url, path, query_params);
    // Event type có thể là EVENT_TYPE_RTDB_GET_RESULT hoặc một EVENT_TYPE_RTDB_QUERY_RESULT riêng
    return rtdb_client_create_http_task(query_url, HTTP_METHOD_GET, EVENT_TYPE_RTDB_QUERY_RESULT, NULL, path);
}
