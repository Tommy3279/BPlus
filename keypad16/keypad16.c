#include <stdio.h>
#include <string.h> // For memset
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "keypad16.h"      // Header của chính component này
#include "box_system.h"  // Header chứa định nghĩa system_event_t, keypad_event_data_t, KEY_EVENT_...
#include "event_pool_manager.h" // Header chứa định nghĩa cho event pool

// --- Định nghĩa nội bộ ---
#define TAG "KEYPAD16" // Đổi TAG để rõ ràng hơn
#define KEYPAD_ROWS 4  // Giả định là 4x4
#define KEYPAD_COLS 4
#define DEFAULT_DEBOUNCE_MS 50
#define DEFAULT_HOLD_MS     500
#define SCAN_INTERVAL_MS    30 // Tăng nhẹ để tránh quá tải CPU, có thể điều chỉnh
#define STACK_SIZE          16384 // Kích thước stack cho task quét

// --- Trạng thái nội bộ của từng phím ---
typedef enum {
    KEY_STATE_IDLE,         // Không nhấn, ổn định
    KEY_STATE_DEBOUNCING,   // Đang trong giai đoạn chống rung (mới nhấn/nhả)
    KEY_STATE_PRESSED,      // Đã nhấn ổn định
    KEY_STATE_HOLD_DETECTED // Đã phát hiện giữ phím (để chỉ gửi event HOLD 1 lần)
} key_internal_state_t;

typedef struct {
    key_internal_state_t state;
    int64_t last_change_time_us; // Thời điểm thay đổi trạng thái vật lý gần nhất (us)
    bool current_physical_state; // Trạng thái vật lý đọc được (true = pressed)
} key_status_t;

// --- Biến toàn cục Static của component ---
// Lưu ý: Sử dụng biến static toàn cục giới hạn khả năng tái sử dụng và tạo nhiều instance.
// Một thiết kế tốt hơn sẽ dùng struct chứa trạng thái và truyền con trỏ handle.
static keypad_config_t g_config;
static TaskHandle_t g_keypad_task_handle = NULL;
static key_status_t g_key_status[KEYPAD_ROWS][KEYPAD_COLS];
static uint32_t g_debounce_us = DEFAULT_DEBOUNCE_MS * 1000; // Lưu bằng micro giây
static uint32_t g_hold_us = DEFAULT_HOLD_MS * 1000;       // Lưu bằng micro giây
static QueueHandle_t g_output_queue = NULL; // Queue do main truyền vào
static bool g_is_initialized = false;

// Bảng ánh xạ ký tự (có thể làm cho nó cấu hình được nếu muốn)
static const char keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

//#define QUEUE_SIZE 10
//#define SCAN_INTERVAL_MS 10
//#define STACK_SIZE 2048

//================================================================================================
// --- Khai báo hàm nội bộ ---
//================================================================================================

static void keypad_scan_task(void *pvParameters);

// --- Tác vụ quét bàn phím ---
static void keypad_scan_task(void *pvParameters) {
    ESP_LOGI(TAG, "Keypad scan task started.");
    int64_t current_time_us;
    
    while (1) {
        if (!g_is_initialized || g_output_queue == NULL) {
            ESP_LOGE(TAG, "Keypad not initialized or queue invalid. Task stopping.");
            vTaskDelete(NULL); // Xóa task nếu không thể hoạt động
            return;
        }

        current_time_us = esp_timer_get_time();

        
        for (int r = 0; r < KEYPAD_ROWS; ++r) { // Vòng lặp qua các HÀNG
            // Kéo hàng hiện tại (r) xuống LOW
            gpio_set_level(g_config.rows[r], 0);
            // esp_rom_delay_us(5); // Delay nhỏ nếu phần cứng yêu cầu để tín hiệu ổn định

            for (int c = 0; c < KEYPAD_COLS; ++c) { // Vòng lặp qua các CỘT
                bool new_physical_state = (gpio_get_level(g_config.cols[c]) == 0);
                key_status_t *key = &g_key_status[r][c]; // Trạng thái của phím [r][c]
                bool state_changed_physically = (new_physical_state != key->current_physical_state);

                keypad_event_type_t determined_event_type = (keypad_event_type_t)-1; // Lưu loại sự kiện xác định được
                bool send_event_flag = false;

                if (state_changed_physically) {
                    key->last_change_time_us = current_time_us;
                    key->current_physical_state = new_physical_state;
                    key->state = KEY_STATE_DEBOUNCING; // Luôn vào DEBOUNCING khi có thay đổi vật lý
                    // ESP_LOGD(TAG, "Key [%d,%d] ('%c') Physical change: %d -> Debouncing", r, c, keymap[r][c], new_physical_state);
                } else {
                    // Trạng thái vật lý không đổi, xử lý state machine nội bộ
                    switch (key->state) {
                        case KEY_STATE_DEBOUNCING:
                            if ((current_time_us - key->last_change_time_us) >= g_debounce_us) {
                                if (key->current_physical_state) { // Debounce thành công -> PRESSED
                                    key->state = KEY_STATE_PRESSED;
                                    key->last_change_time_us = current_time_us; // Reset thời gian để tính HOLD
                                    determined_event_type = KEYPAD_PRESSED;
                                    send_event_flag = true;
                                    // ESP_LOGD(TAG, "Key [%d,%d] ('%c') Debounced -> PRESSED", r, c, keymap[r][c]);
                                } else { // Debounce thành công -> IDLE (Released)
                                    key->state = KEY_STATE_IDLE;
                                    determined_event_type = KEYPAD_RELEASED;
                                    send_event_flag = true;
                                    // ESP_LOGD(TAG, "Key [%d,%d] ('%c') Debounced -> IDLE (Released)", r, c, keymap[r][c]);
                                }
                            }
                            break;

                        case KEY_STATE_PRESSED:
                            // Kiểm tra nếu phím được nhả ra TRƯỚC khi đủ thời gian HOLD
                            if (!key->current_physical_state) {
                                key->state = KEY_STATE_DEBOUNCING; // Quay lại debounce cho sự kiện RELEASE
                                key->last_change_time_us = current_time_us; // Cập nhật thời điểm thay đổi
                                // determined_event_type = KEYPAD_RELEASED; // Sẽ được xử lý ở vòng lặp DEBOUNCING tiếp theo
                                // send_event_flag = true;
                                // ESP_LOGD(TAG, "Key [%d,%d] ('%c') Released during PRESSED -> Debouncing for RELEASE",r,c,keymap[r][c]);
                            }
                            // Kiểm tra nếu đủ thời gian HOLD
                            else if ((current_time_us - key->last_change_time_us) >= g_hold_us) {
                                key->state = KEY_STATE_HOLD_DETECTED; // Chuyển trạng thái để chỉ gửi event HOLD 1 lần
                                determined_event_type = KEYPAD_HOLD;
                                send_event_flag = true;
                                // ESP_LOGD(TAG, "Key [%d,%d] ('%c') Hold Detected",r,c, keymap[r][c]);
                            }
                            break;

                        case KEY_STATE_HOLD_DETECTED:
                             // Nếu phím được nhả ra SAU KHI đã phát hiện HOLD
                            if (!key->current_physical_state) {
                                key->state = KEY_STATE_DEBOUNCING; // Quay lại debounce cho sự kiện RELEASE
                                key->last_change_time_us = current_time_us; // Cập nhật thời điểm thay đổi
                                // determined_event_type = KEYPAD_RELEASED; // Sẽ được xử lý ở vòng lặp DEBOUNCING tiếp theo
                                // send_event_flag = true;
                                // ESP_LOGD(TAG, "Key [%d,%d] ('%c') Released after HOLD_DETECTED -> Debouncing for RELEASE", r,c,keymap[r][c]);
                            }
                            break;

                        case KEY_STATE_IDLE:
                            // Không làm gì nếu phím đang IDLE và không có thay đổi vật lý
                            break;
                    } // kết thúc switch(key->state)
                } // kết thúc else của if (state_changed_physically)

                // Nếu cờ send_event_flag được bật và đã xác định được loại sự kiện
                if (send_event_flag && determined_event_type != (keypad_event_type_t)-1) {
                    system_event_t *evt_to_send = event_pool_acquire_buffer(0); // *** Acquire buffer TẠI ĐÂY ***
                    if (evt_to_send != NULL) {
                        // Điền thông tin sự kiện
                        evt_to_send->type = EVENT_TYPE_KEYPAD;
                        evt_to_send->timestamp = (uint32_t)(current_time_us / 1000);
                        evt_to_send->data.keypad.row = r;
                        evt_to_send->data.keypad.col = c;
                        evt_to_send->data.keypad.key_char = keymap[r][c];
                        evt_to_send->data.keypad.type = determined_event_type; // Gán loại sự kiện đã xác định

                        // Gửi vào queue
                        if (xQueueSend(g_output_queue, &evt_to_send, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "Output queue full. Keypad event ['%c'] type %d dropped.",
                                     keymap[r][c], determined_event_type);
                            event_pool_release_buffer(evt_to_send); // Trả lại buffer nếu không gửi được
                        } else {
                           // ESP_LOGD(TAG, "Keypad event for '%c' (type %d) sent to queue.", keymap[r][c], determined_event_type);
                        }
                    } else {
                        ESP_LOGW(TAG, "Failed to acquire event buffer for keypad event ('%c' type %d)", keymap[r][c], determined_event_type);
                    }
                } // kết thúc if (send_event_flag)
            } // kết thúc vòng lặp CỘT (c)

            // Trả hàng hiện tại (r) về HIGH (trạng thái nghỉ)
            gpio_set_level(g_config.rows[r], 1);
        } // kết thúc vòng lặp HÀNG (r)

        // Đợi khoảng thời gian quét tiếp theo
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    } // kết thúc while(1)
}

//===============================================================================================
// --- Hàm công khai --- khởi tạo driver và task quét bàn phím
//===============================================================================================
esp_err_t keypad_init(const keypad_config_t *config, QueueHandle_t output_queue) { // code mới
    // if (!config || !callback) { // code cũ
    if (!config || !output_queue) { // code mới
        ESP_LOGE(TAG, "Invalid arguments provided to keypad_init");
        return ESP_ERR_INVALID_ARG;
    }
    if (g_is_initialized) {
        ESP_LOGW(TAG, "Keypad already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    // Set initialized flag to false during initialization
        g_is_initialized = false;

    // Lưu cấu hình và queue handle
    g_config = *config; // Sao chép cấu hình
    g_output_queue = output_queue; // Lưu handle của queue
    // g_event_callback = callback; // code cũ (loại bỏ)

    // Đặt giá trị debounce/hold mặc định (có thể ghi đè sau bằng hàm set)
    g_debounce_us = DEFAULT_DEBOUNCE_MS * 1000;
    g_hold_us = DEFAULT_HOLD_MS * 1000;

    esp_err_t ret;

    // --- Cấu hình chân GPIO cho các hàng (OUTPUT) ---
    gpio_config_t row_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    uint64_t row_pin_mask = 0;
    for (int i = 0; i < KEYPAD_ROWS; ++i) {
         // Sử dụng g_config thay vì config trực tiếp
        if (g_config.rows[i] >= 0 && g_config.rows[i] < GPIO_NUM_MAX) {
             row_pin_mask |= (1ULL << g_config.rows[i]);
        } else {
            ESP_LOGE(TAG, "Invalid row pin specified: %d", g_config.rows[i]);
            g_output_queue = NULL; // Clear queue handle on error
            return ESP_ERR_INVALID_ARG;
        }
    }
    row_conf.pin_bit_mask = row_pin_mask;
    ret = gpio_config(&row_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure row GPIOs: %s", esp_err_to_name(ret));
        g_output_queue = NULL;
        return ret;
    }
    // Đặt tất cả các hàng lên mức HIGH ban đầu (trạng thái nghỉ)
    for (int i = 0; i < KEYPAD_ROWS; ++i) {
        gpio_set_level(g_config.rows[i], 1);
    }

    // --- Cấu hình chân GPIO cho các cột (INPUT + PULLUP) ---
    gpio_config_t col_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Sử dụng pull-up nội bộ
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE // Không dùng ngắt, quét chủ động
    };
    uint64_t col_pin_mask = 0;
    for (int i = 0; i < KEYPAD_COLS; ++i) {
         // Sử dụng g_config thay vì config trực tiếp
         if (g_config.cols[i] >= 0 && g_config.cols[i] < GPIO_NUM_MAX) {
             col_pin_mask |= (1ULL << g_config.cols[i]);
         } else {
            ESP_LOGE(TAG, "Invalid column pin specified: %d", g_config.cols[i]);
            // Cần hủy cấu hình row pins đã làm trước đó nếu có lỗi ở đây
            // (Tạm bỏ qua để đơn giản, nhưng cần làm trong code thực tế)
            g_output_queue = NULL;
            return ESP_ERR_INVALID_ARG;
        }
    }
    col_conf.pin_bit_mask = col_pin_mask;
    ret = gpio_config(&col_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure column GPIOs: %s", esp_err_to_name(ret));
        g_output_queue = NULL;
        return ret;
    }

    // Khởi tạo trạng thái các phím
    memset(g_key_status, 0, sizeof(g_key_status)); // state = 0 (IDLE), times = 0

    // Set initialized flag BEFORE creating task
        g_is_initialized = true;

    // Tạo tác vụ quét bàn phím
    BaseType_t task_created = xTaskCreate(
        keypad_scan_task,       // Hàm thực thi tác vụ
        "keypad_scan_task",     // Tên tác vụ
        STACK_SIZE,             // Kích thước stack (bytes)
        NULL,                   // Tham số truyền vào tác vụ
        6,                      // Độ ưu tiên tác vụ (có thể cần điều chỉnh)
        &g_keypad_task_handle   // Handle của tác vụ
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create keypad scan task");
        g_is_initialized = false;  // Reset initialization flag
        g_output_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Keypad initialized successfully.");
    return ESP_OK;
}

//===============================================================================================
// Hàm hủy khởi tạo (nên thêm vào)
//===============================================================================================
// Hàm này sẽ dừng task quét và giải phóng tài nguyên nếu cần
esp_err_t keypad_deinit(void) {
    if (!g_is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing keypad...");

    // Dừng và xóa task quét
    if (g_keypad_task_handle != NULL) {
        vTaskDelete(g_keypad_task_handle);
        g_keypad_task_handle = NULL;
    }

    // (Tùy chọn) Reset cấu hình GPIO về mặc định
    // Việc này có thể cần thiết nếu các chân GPIO cần được sử dụng cho mục đích khác
    // Ví dụ: lặp qua g_config.rows và g_config.cols và gọi gpio_reset_pin()

    // Đặt lại các biến trạng thái
    g_output_queue = NULL;
    memset(g_key_status, 0, sizeof(g_key_status));
    g_is_initialized = false;

    ESP_LOGI(TAG, "Keypad deinitialized.");
    return ESP_OK;
}

//===============================================================================================
// --- Hàm công khai để thiết lập thời gian debounce và hold ---
//===============================================================================================
// Hàm này cho phép người dùng điều chỉnh thời gian debounce và hold sau khi khởi tạo

void keypad_set_debounce_time(uint32_t debounce_ms) {
    if (!g_is_initialized) {
         ESP_LOGE(TAG, "Keypad not initialized. Cannot set debounce time.");
         return;
    }
    if (debounce_ms > 0) {
        // Cần cơ chế bảo vệ truy cập đa luồng nếu `main` gọi hàm này
        // trong khi task quét đang chạy. Tạm thời bỏ qua để đơn giản.
        g_debounce_us = debounce_ms * 1000;
        ESP_LOGI(TAG, "Debounce time set to %lu ms", debounce_ms);
    }
}

void keypad_set_hold_time(uint32_t hold_ms) {
     if (!g_is_initialized) {
         ESP_LOGE(TAG, "Keypad not initialized. Cannot set hold time.");
         return;
    }
    // Hold time phải lớn hơn debounce time
    if (hold_ms * 1000 > g_debounce_us) {
        // Cần cơ chế bảo vệ truy cập đa luồng
        g_hold_us = hold_ms * 1000;
        ESP_LOGI(TAG, "Hold time set to %lu ms", hold_ms);
    } else {
        ESP_LOGW(TAG, "Hold time (%lu ms) must be greater than debounce time (%lu ms)", hold_ms, g_debounce_us / 1000);
    }
}