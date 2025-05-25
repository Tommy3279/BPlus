#ifndef KEYPAD16_H
#define KEYPAD16_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
#include "box_system.h"

typedef struct {
    int8_t rows[4];           // Row GPIO pins
    int8_t cols[4];           // Column GPIO pins
    uint8_t num_rows;         // Number of rows
    uint8_t num_cols;         // Number of columns
} keypad_config_t;


/* Đã định nhĩa trong box_system.h
// Định nghĩa các trạng thái của phím, sẽ được sử dụng trong hàm xử lý sự kiện
typedef enum {
    KEY_EVENT_PRESSED,
    KEY_EVENT_RELEASED,
    KEY_EVENT_HOLD,
} keypad_event_type_t;

// Định nghĩa cấu trúc sự kiện phím
typedef struct {
    uint8_t row;                // Hàng của phím (0-3)
    uint8_t col;                // Cột của phím (0-3)
    char key_char;              // Ký tự tương ứng (nếu có)
    keypad_event_type_t type;   // Loại sự kiện (Pressed, Released, Hold)
} keypad_event_t;
*/

// Định nghĩa kiểu hàm callback cho sự kiện phím

//typedef void (*keypad_event_callback_t)(keypad_event_t event);
/*
// --- Định nghĩa chân GPIO sử dụng cho keypad ---
#define ROW0_PIN GPIO_NUM_4
#define ROW1_PIN GPIO_NUM_18
#define ROW2_PIN GPIO_NUM_19
#define ROW3_PIN GPIO_NUM_21

#define COL0_PIN GPIO_NUM_27
#define COL1_PIN GPIO_NUM_26
#define COL2_PIN GPIO_NUM_25
#define COL3_PIN GPIO_NUM_33


#define TAG "KEYPAD"

// --- Cấu hình mặc định ---
#define KEYPAD_ROWS 4
#define KEYPAD_COLS 4
#define DEFAULT_DEBOUNCE_MS 50
#define DEFAULT_HOLD_MS     500
#define SCAN_INTERVAL_MS    10 // Quét mỗi 10ms để giảm độ trễ

// --- Trạng thái nội bộ của từng phím ---
typedef enum {
    KEY_BOX_STATE_STANDBY,         // Không nhấn
    KEY_STATE_DEBOUNCING,   // Đang trong giai đoạn chống rung (mới nhấn/nhả)
    KEY_STATE_PRESSED,      // Đã nhấn ổn định
    KEY_STATE_HOLD_DETECTED // Đã phát hiện giữ phím (để tránh gửi sự kiện hold liên tục)
} key_internal_state_t;

typedef struct {
    key_internal_state_t state;
    int64_t last_change_time_us; // Thời điểm thay đổi trạng thái gần nhất (us)
    bool current_physical_state; // Trạng thái vật lý đọc được (true = pressed)
} key_status_t;

// --- Biến toàn cục của component ---
static keypad_config_t g_config;
//static keypad_event_callback_t g_event_callback = NULL;
static TaskHandle_t g_keypad_task_handle = NULL;
static key_status_t g_key_status[KEYPAD_ROWS][KEYPAD_COLS];
static uint32_t g_debounce_us;
static uint32_t g_hold_us;


// Bảng ánh xạ ký tự (tùy chọn, có thể thay đổi)
static const char keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

*/
/**
 * @brief Khởi tạo driver và task quét bàn phím.
 *
 * @param config Cấu hình chân GPIO cho bàn phím.
 * @param output_queue Queue mà component sẽ gửi sự kiện keypad_event_t vào.
 * Queue này phải được tạo trước khi gọi hàm này.
 * @return esp_err_t ESP_OK nếu thành công, lỗi nếu thất bại.
 */
// esp_err_t keypad_init(const keypad_config_t *config, keypad_event_callback_t callback); // code cũ
esp_err_t keypad_init(const keypad_config_t *config, QueueHandle_t output_queue); // code mới

/**
 * @brief (Tùy chọn) Thiết lập thời gian debounce (ms). Phải gọi sau keypad_init.
 *
 * @param debounce_ms Thời gian debounce mới (tối thiểu nên > SCAN_INTERVAL_MS).
 */
void keypad_set_debounce_time(uint32_t debounce_ms);

/**
 * @brief (Tùy chọn) Thiết lập thời gian giữ phím (ms). Phải gọi sau keypad_init.
 *
 * @param hold_ms Thời gian giữ phím mới (phải lớn hơn debounce_ms).
 */
void keypad_set_hold_time(uint32_t hold_ms);

/**
 * @brief Dừng task quét và giải phóng tài nguyên (nếu cần).
 *
 * @return esp_err_t ESP_OK nếu thành công.
 */
esp_err_t keypad_deinit(void);


#endif // KEYPAD16_H