// components/rtdb_client/rtdb_client.h
#ifndef RTDB_CLIENT_H
#define RTDB_CLIENT_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#include "nvs.h"
#include "nvs_flash.h"

// Biến static để lưu trữ token sau khi xác thực thành công
// Kích thước cần đủ lớn cho Firebase tokens
#define MAX_ID_TOKEN_LEN_INTERNAL 2048 // Cho s_id_token nội bộ
#define MAX_REFRESH_TOKEN_LEN_INTERNAL 1024

#define MAX_UID_LEN_INTERNAL 64
#define MAX_API_KEY_LEN_INTERNAL 128

// NVS Namespace and Keys
#define NVS_NAMESPACE_AUTH "fb_auth_v1" // Dùng namespace mới để tránh xung đột với dữ liệu NVS cũ nếu có
#define NVS_KEY_UID "uid"
#define NVS_KEY_ID_TOKEN "id_token"       // Có thể không cần lưu ID token cũ, chỉ cần refresh token
#define NVS_KEY_REFRESH_TOKEN "refresh_token"
#define NVS_KEY_API_KEY "api_key"         // Lưu API key để dùng cho refresh



// --- Public Function Prototypes ---



/**
 * @brief Khởi tạo component rtdb_client.
 *
 * @param main_event_queue Handle đến master event queue của hệ thống.
 * @return esp_err_t ESP_OK nếu thành công, ngược lại là mã lỗi.
 */
esp_err_t rtdb_client_init(QueueHandle_t main_event_queue); // Queue này giờ sẽ nhận system_event_t*

/**
 * @brief Thực hiện một yêu cầu HTTP GET kiểm tra đến một URL công cộng (không phải Firebase).
 * Kết quả sẽ được gửi về master_event_queue với type EVENT_TYPE_HTTP_TEST_RESULT.
 *
 * @param url URL để kiểm tra (ví dụ: "https://www.howsmyssl.com/a/check").
 * @return esp_err_t ESP_OK nếu yêu cầu được gửi đi thành công (không phải kết quả HTTP),
 * ngược lại là mã lỗi.
 */
esp_err_t rtdb_client_perform_http_test(const char* url);

// Các hàm khác sẽ được thêm sau (Firebase Auth, GET, PUT...)
// esp_err_t rtdb_client_firebase_anonymous_auth(void);
// esp_err_t rtdb_client_get_data(const char* rtdb_path);
// esp_err_t rtdb_client_put_data(const char* rtdb_path, const char* json_payload);


/**
 * @brief Khởi động quá trình xác thực cho thiết bị.
 * Ưu tiên đọc thông tin xác thực từ NVS và làm mới ID Token.
 * Nếu không thành công, sẽ thực hiện đăng nhập ẩn danh mới.
 * Kết quả sẽ được gửi về master_event_queue với type EVENT_TYPE_FIREBASE_AUTH_RESULT.
 *
 * @param web_api_key Web API Key của dự án Firebase của bạn.
 * @return esp_err_t ESP_OK nếu quá trình khởi tạo yêu cầu xác thực được bắt đầu,
 * ngược lại là mã lỗi.
 */
esp_err_t rtdb_client_authenticate_device(const char* web_api_key);

/**
 * @brief Yêu cầu làm mới ID Token một cách tường minh (nếu cần).
 * Sẽ sử dụng Refresh Token đã lưu trong NVS.
 * Kết quả sẽ được gửi về master_event_queue với type EVENT_TYPE_FIREBASE_AUTH_RESULT.
 *
 * @return esp_err_t ESP_OK nếu yêu cầu làm mới được bắt đầu, ngược lại là mã lỗi.
 */
esp_err_t rtdb_client_force_refresh_token(void);




esp_err_t _nvs_read_string(nvs_handle_t nvs_handle_param, const char* key, char* out_value, size_t max_len);
esp_err_t _nvs_write_string(nvs_handle_t nvs_handle_param, const char* key, const char* value);

// (Hàm rtdb_client_firebase_anonymous_auth sẽ được dùng nội bộ hoặc có thể bỏ nếu không cần gọi trực tiếp từ bên ngoài nữa)
// esp_err_t rtdb_client_firebase_anonymous_auth(const char* web_api_key);


esp_err_t rtdb_client_perform_http_test(const char* url);
esp_err_t rtdb_client_get_data(const char* rtdb_base_url, const char* path);
esp_err_t rtdb_client_put_data(const char* rtdb_base_url, const char* path, const char* json_payload);
esp_err_t rtdb_client_patch_data(const char* rtdb_base_url, const char* path, const char* json_payload);
esp_err_t rtdb_client_delete_data(const char* rtdb_base_url, const char* path);
esp_err_t rtdb_client_query_data(const char* rtdb_base_url, const char* path, const char* query_params);

const char* rtdb_client_get_id_token(void);
const char* rtdb_client_get_uid(void); // Hàm để lấy UID đã lưu

#endif // RTDB_CLIENT_H
