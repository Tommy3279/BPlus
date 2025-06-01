#ifndef BOX_SYSTEM_H
#define BOX_SYSTEM_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
//#include "lock_controller_driver.h"
//#include <rtdb_client.h> // Để sử dụng các định nghĩa trong rtdb_client.h

#define RS485_MAX_PACKET_SIZE 64 // Kích thước buffer nhận tối đa, đủ lớn cho các gói tin

// --- Loại Sự kiện Hệ thống Chính ---
typedef enum {
    EVENT_TYPE_KEYPAD,             // Sự kiện từ bàn phím
    EVENT_TYPE_TIMER_IDLE,         // Sự kiện từ idle timer
    EVENT_TYPE_WIFI_PROVISION,     // Sự kiện từ tiến trình provisioning
    EVENT_TYPE_PROVISIONING_REQUEST, // Sự kiện yêu cầu vào chế độ provisioning (từ combo key hold)
    EVENT_TYPE_WIFI_CONNECTED,
    EVENT_TYPE_WIFI_DISCONNECTED, // Giữ lại nếu bạn muốn xử lý mất kết nối sau này
    EVENT_TYPE_WIFI_CONNECT_FAIL,  // !!! MỚI: Sự kiện báo kết nối ban đầu thất bại !!!
    EVENT_TYPE_PHONE_VALIDATE_REQUESTED, // Sự kiện xác thực số điện thoại (có thể từ keypad hoặc từ server)
    EVENT_TYPE_COMPARTMENT_OPEN_REQUEST, // Sự kiện chọn ô tủ (có thể từ keypad hoặc từ server)
    // Thêm các event type mới cho rtdb_client
    EVENT_TYPE_HTTP_TEST_REQUEST,     // Yêu cầu thực hiện HTTP test
    EVENT_TYPE_HTTP_TEST_RESULT,      // Kết quả từ HTTP test
    EVENT_TYPE_FIREBASE_AUTH_REQUEST, // Yêu cầu xác thực Firebase
    EVENT_TYPE_FIREBASE_AUTH_RESULT,  // Kết quả xác thực Firebase
    EVENT_TYPE_RTDB_GET_REQUEST,    // Event yêu cầu đọc dữ liệu
    EVENT_TYPE_RTDB_GET_RESULT,     // Event chứa kết quả đọc

    EVENT_TYPE_RTDB_PUT_REQUEST,    // Event yêu cầu ghi (PUT)
    EVENT_TYPE_RTDB_PUT_RESULT,     // Event chứa kết quả ghi (PUT)

    EVENT_TYPE_RTDB_PATCH_REQUEST,  // Event yêu cầu cập nhật (PATCH)
    EVENT_TYPE_RTDB_PATCH_RESULT,   // Event chứa kết quả cập nhật (PATCH)

    EVENT_TYPE_RTDB_DELETE_REQUEST, // Event yêu cầu xóa
    EVENT_TYPE_RTDB_DELETE_RESULT,  // Event chứa kết quả xóa

    EVENT_TYPE_RTDB_QUERY_REQUEST,  // Event yêu cầu query
    EVENT_TYPE_RTDB_QUERY_RESULT,   // Event chứa kết quả query (tương tự GET_RESULT)

    EVENT_TYPE_TOKEN_REFRESH_NEEDED, // Event báo cần làm mới token
    EVENT_TYPE_TOKEN_REFRESH_RESULT, // Event báo kết quả làm mới token
    
    EVENT_TYPE_RS485_ERROR,     // Loại sự kiện mới cho lỗi RS485

    EVENT_TYPE_LOCK_COMMAND_TRIGGER, // Kích hoạt lệnh mở khóa
    EVENT_TYPE_LOCK_RESPONSE_RECEIVED, // Phản hồi từ bo điều khiển khóa

    EVENT_TYPE_LOCK_COMMAND_FAILED, // Lệnh mở khóa thất bại
    EVENT_TYPE_SINGLE_LOCK_RESULT,  // Phản hồi từ controller sau lệnh mở

    EVENT_TYPE_DOOR_STATUS_CHANGED_OPEN, // Trạng thái cửa thay đổi (do bo điều khiển upload)
    EVENT_TYPE_DOOR_STATUS_CHANGED_CLOSE,
    // ... Thêm các loại sự kiện chính khác ...
} system_event_type_t;


// --- Định nghĩa các mã lỗi RS485 ---
typedef enum {
    RS485_ERR_NONE = 0,
    RS485_ERR_TIMEOUT,                  // Lỗi timeout khi chờ dữ liệu
    RS485_ERR_INCOMPLETE_FRAME,         // Gói tin bị cắt ngắn (ví dụ: nhỏ hơn 9 byte)
    RS45_ERR_FRAME_LENGTH_MISMATCH,     // Số byte nhận được không khớp với Frame Length báo cáo
    RS485_ERR_CHECKSUM_MISMATCH,        // Lỗi checksum
    RS485_ERR_INVALID_DATA_LEN,         // Chiều dài dữ liệu trong gói tin không hợp lệ cho lệnh cụ thể
    RS485_ERR_INVALID_START_CODE        // Start Code không đúng
} rs485_error_code_t;

// --- Cấu trúc dữ liệu cho lỗi RS485 ---
typedef struct {
    rs485_error_code_t error_code; // Mã lỗi cụ thể
    uint8_t board_id;              // Board ID của gói tin (nếu có thể xác định)
    uint8_t command_word;          // Command Word của gói tin (nếu có thể xác định)
    // Có thể thêm dữ liệu thô của gói tin nếu cần debug sâu
    // uint8_t raw_data[RS485_MAX_PACKET_SIZE];
    // size_t raw_data_len;
} rs485_error_data_t;

// --- Trạng thái Sự kiện WiFi Provisioning ---
typedef enum {
    WIFI_PROVISION_START,   // Bắt đầu
    WIFI_PROVISION_SUCCESS, // Thành công (nhận được credential)
    WIFI_PROVISION_FAIL,    // Thất bại (lỗi xác thực hoặc không tìm thấy AP)
    // WIFI_PROVISION_END // Có thể không cần nếu SUCCESS/FAIL đã đủ
} wifi_prov_status_t; // Đổi tên từ code gốc của bạn cho nhất quán

// --- Dữ liệu Sự kiện WiFi Provisioning ---
typedef struct {
    wifi_prov_status_t status;      // Trạng thái tùy chỉnh ở trên
    char ssid[33];                  // Lưu SSID nếu thành công (32 ký tự + null)
    // esp_err_t error_code;        // (Tùy chọn) Thêm mã lỗi ESP-IDF nếu cần
} wifi_prov_event_data_t; // Đổi tên từ code gốc của bạn cho nhất quán




// --- Kích thước tối đa cho các payload ---
#define RTDB_HTTP_RESPONSE_MAX_LEN 1024 // Cho HTTP test response (howsmyssl)
#define RTDB_ID_TOKEN_MAX_LEN 1200
#define RTDB_REFRESH_TOKEN_MAX_LEN 512
#define RTDB_UID_MAX_LEN 128
#define RTDB_ERROR_MSG_MAX_LEN 256 // Cho thông báo lỗi từ Firebase hoặc HTTP
#define RTDB_PATH_MAX_LEN 256
#define RTDB_RESPONSE_PREVIEW_MAX_LEN 512 // Cho preview của RTDB GET response





// ==========================================================================
// Định nghĩa Sự kiện Hệ thống (System Events)
// ==========================================================================

// --- Loại Sự kiện Keypad ---
typedef enum {
    KEYPAD_PRESSED,
    KEYPAD_RELEASED,
    KEYPAD_HOLD
    // KEYPAD_TIMEOUT // Code cũ (loại bỏ nếu không dùng)
} keypad_event_type_t; // Code mới: Định nghĩa enum riêng

// --- Dữ liệu Sự kiện Keypad ---
typedef struct {
    uint8_t row;
    uint8_t col;
    char key_char;
    // enum { ... } type; // Code cũ: enum lồng trong struct
    keypad_event_type_t type; // Code mới: Sử dụng enum đã định nghĩa ở trên
} keypad_event_data_t;


// Cấu trúc dữ liệu cho kết quả của HTTP Test
typedef struct {
    esp_err_t status;
    int http_status_code;
    char response_data[RTDB_HTTP_RESPONSE_MAX_LEN]; // << Đổi thành mảng cố định
} rtdb_http_test_result_data_t;


// Cấu trúc dữ liệu cho kết quả xác thực Firebase
typedef struct {
    esp_err_t status;
    int http_status_code;
    char id_token[RTDB_ID_TOKEN_MAX_LEN];           // << Đổi thành mảng cố định
    char refresh_token[RTDB_REFRESH_TOKEN_MAX_LEN]; // << Đổi thành mảng cố định
    char uid[RTDB_UID_MAX_LEN];                     // << Đổi thành mảng cố định
    char error_message[RTDB_ERROR_MSG_MAX_LEN];     // << Đổi thành mảng cố định
    bool parse_success;
} rtdb_firebase_auth_result_data_t;

typedef struct {
    esp_err_t status;
    int http_status_code;
    char path[RTDB_PATH_MAX_LEN];                   // << Đổi thành mảng cố định
    char response_preview[RTDB_RESPONSE_PREVIEW_MAX_LEN]; // << Đổi thành mảng cố định
} rtdb_get_result_data_t;

typedef struct {
    esp_err_t status;
    int http_status_code;
    char path[RTDB_PATH_MAX_LEN];                   // << Đổi thành mảng cố định
    // Có thể thêm các trường khác nếu PUT/PATCH trả về dữ liệu
} rtdb_write_result_data_t; // Dùng cho PUT, PATCH, DELETE


//thêm các cấu trúc dữ liệu cho các sự kiện khác nếu cần



// Định nghĩa cấu trúc phản hồi của lock controller
typedef struct {
    uint8_t start_code[4]; // 0x57 0x4B 0x4C 0x59
    uint8_t frame_length;
    uint8_t board_id;
    uint8_t command_word; // 0x86 for open single, 0x87 for open multiple
    uint8_t command_status;  // 0x00 for success, 0xFF for fail (for 0x86 response)
    uint8_t lock_channel; // Kênh khóa bị ảnh hưởng
    uint8_t lock_status;  // Trạng thái khóa sau lệnh (0x00: mở thành công, 0x01: mở thất bại)
    uint8_t checksum;
} lock_controller_response_t;



// Dữ liệu cho các event yêu cầu RTDB (nếu cần truyền thêm thông tin)
typedef struct {
    char path[RTDB_PATH_MAX_LEN];
    // char query_params[SOME_MAX_LEN]; // Cho QUERY_REQUEST
} rtdb_request_path_data_t;

typedef struct {
    char path[RTDB_PATH_MAX_LEN];
    char payload[1024]; // Kích thước payload tối đa cho PUT/PATCH, điều chỉnh nếu cần
} rtdb_request_write_data_t;



// --- Cấu trúc Sự kiện Hệ thống Chính ---
// Union chứa dữ liệu cho từng loại sự kiện
// Các thành viên rtdb_..._result sẽ sử dụng kiểu đã được cập nhật từ rtdb_client.h
typedef union {
    keypad_event_data_t keypad;
    wifi_prov_event_data_t wifi;

    rtdb_http_test_result_data_t http_test_result;
    rtdb_firebase_auth_result_data_t firebase_auth_result;

    // Dùng chung struct cho các request có path
    rtdb_request_path_data_t rtdb_path_request; // Cho GET, DELETE, QUERY
    rtdb_request_write_data_t rtdb_write_request; // Cho PUT, PATCH

    rtdb_get_result_data_t rtdb_get_result;       // Cũng dùng cho QUERY_RESULT
    rtdb_write_result_data_t rtdb_write_result;   // Cũng dùng cho PUT, PATCH, DELETE result
    
    uint8_t lock_channel_to_open;
    lock_controller_response_t controller_response;
    rs485_error_data_t rs485_error;
    // Thêm các struct khác nếu cần, ví dụ cho token refresh result
    // rtdb_token_refresh_result_data_t token_refresh_result;
} system_event_data_t;

// Cấu trúc sự kiện hệ thống chính
typedef struct {
    system_event_type_t type;
    uint32_t timestamp;
    system_event_data_t data; // Union này giờ sẽ lớn hơn đáng kể
} system_event_t;


// ==========================================================================
// Định nghĩa Trạng thái Hệ thống (State Machine)
// ==========================================================================
typedef enum {
    BOX_STATE_INITIALIZING,
    BOX_STATE_WIFI_CONNECTING,    // Chờ kết nối WiFi (STA) thành công
    BOX_STATE_STANDBY,             // Chờ 
    BOX_STATE_SELECT_COMPARTMENT,  // Màn hình chọn ô tủ
    BOX_STATE_INPUT_PHONE_NUM,     // Nhập SĐT
    BOX_STATE_VALIDATING_PHONE_NUM,// Đang xác thực SĐT
    BOX_STATE_OPENING_COMPARTMENT, // Đang mở tủ (nếu cần trạng thái riêng)
    BOX_STATE_PROCESSING_ORDER,    // Đang xử lý đơn hàng (nếu cần)
    BOX_STATE_WIFI_PROVISIONING,    // Chế độ cấu hình WiFi
    BOX_STATE_FAULT
    // Thêm các trạng thái khác
} box_state_t;



// --- Khai báo Task ---
//void keypad_scan_task(void *pvParameter);
void master_event_handler(system_event_t *event);

static QueueHandle_t g_master_event_queue_ptr = NULL;

#endif /* BOX_SYSTEM_H */
//==============================================================

