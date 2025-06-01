#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // Thêm queue.h
#include "freertos/semphr.h" // Cho semaphore nếu dùng (queue thường đã đủ thread-safe cho mục đích này)

#include <string.h> // Thêm string.h cho memset
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "rtdb_client.h"
#include "esp_netif_sntp.h" // Cho ESP-IDF 5.0 trở lên
#include "esp_wifi.h"    // Cần cho ví dụ trigger sau khi có IP
#include "esp_event.h"   // Cần cho ví dụ trigger sau khi có IP
#include <esp_ghota.h>   //OTA
#include "esp_system.h"  // Cần cho esp_restart()
#include "cJSON.h"      // Thư viện JSON để xử lý dữ liệu JSON

// Include các file header của dự án
#include "box_system.h"   // Chứa định nghĩa system_event_t, box_state_t, KEY_EVENT_...
#include "keypad16.h"     // Component keypad đã sửa đổi theo Phương án 1
#include "compartments.h" // Chứa định nghĩa compartment_t, NUM_COMPARTMENTS, các trạng thái compartment
#include "display.h"    // Chứa định nghĩa cho LCD (ST7920)
#include "wifi_manager.h"   
#include "event_pool_manager.h"
#include "lock_controller_driver.h"

// Khai báo cho event pool
#define EVENT_POOL_SIZE 10 // Số lượng event trong pool
static system_event_t event_pool[EVENT_POOL_SIZE];
static QueueHandle_t free_event_slots_queue;
QueueHandle_t master_event_queue; // Queue giờ sẽ chứa system_event_t*

// Forward declaration of the GHOTA event handler
static void ghota_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);


static lock_controller_response_t g_open_single_lock_response;

#define TAG_MAIN "MAIN_APP"

//==========================================================================
// Hàm cho event pool
//==========================================================================
// Hàm khởi tạo event pool (gọi một lần trong app_main trước khi tạo master_event_queue)
void event_pool_init(void) {
    free_event_slots_queue = xQueueCreate(EVENT_POOL_SIZE, sizeof(system_event_t*));
    if (free_event_slots_queue == NULL) {
        ESP_LOGE(TAG_MAIN, "Failed to create free_event_slots_queue!");
        // Xử lý lỗi nghiêm trọng
        return;
    }
    for (int i = 0; i < EVENT_POOL_SIZE; i++) {
        // Gửi con trỏ của từng event trong pool vào queue các slot rảnh
        system_event_t* event_ptr = &event_pool[i];
        if (xQueueSend(free_event_slots_queue, &event_ptr, 0) != pdPASS) {
            ESP_LOGE(TAG_MAIN, "Failed to populate free_event_slots_queue at index %d", i);
        }
    }
    ESP_LOGI(TAG_MAIN, "Event pool initialized with %d slots.", EVENT_POOL_SIZE);
}

// Hàm "mượn" một buffer event từ pool
system_event_t* event_pool_acquire_buffer(TickType_t timeout) {
    system_event_t* event_ptr = NULL;
    if (free_event_slots_queue != NULL) {
        if (xQueueReceive(free_event_slots_queue, &event_ptr, timeout) == pdPASS) {
            // Xóa sạch buffer trước khi trả về để tránh dữ liệu cũ
            if (event_ptr) {
                memset(event_ptr, 0, sizeof(system_event_t));
            }
            return event_ptr;
        } else {
            ESP_LOGW(TAG_MAIN, "Failed to acquire event buffer from pool (timeout or queue empty).");
        }
    } else {
        ESP_LOGE(TAG_MAIN, "Free event slots queue not initialized!");
    }
    return NULL; // Trả về NULL nếu không mượn được
}

// Hàm "trả" một buffer event về pool
void event_pool_release_buffer(system_event_t* event_ptr) {
    if (free_event_slots_queue != NULL && event_ptr != NULL) {
        // Không cần kiểm tra có thuộc pool không nếu logic acquire/release đúng
        if (xQueueSend(free_event_slots_queue, &event_ptr, 0) != pdPASS) {
            ESP_LOGE(TAG_MAIN, "Failed to release event buffer back to pool!");
        }
    } else { ESP_LOGW(TAG_MAIN, "Attempted to release NULL event_ptr or free_event_slots_queue not init."); }
}
// --- End Event Pool Manager ---
//==========================================================================


// --- Biến cục bộ cho logic state machine ---
#define PHONE_MAX_LENGTH 11 // Độ dài tối đa SĐT + 1 (cho null terminator)
static char phone_number[PHONE_MAX_LENGTH] = {0}; // Lưu SĐT đang nhập
static int phone_length = 0; // Độ dài SĐT hiện tại
static char selected_compartment = '\0'; // Ô tủ đang được chọn
box_state_t current_state = BOX_STATE_STANDBY; // Trạng thái hiện tại của state machine

// --- Timer cho trạng thái không hoạt động (Idle) ---
#define IDLE_TIMEOUT_MS 30000 // Ví dụ: 30 giây không hoạt động thì về Standby
static TimerHandle_t idle_timer = NULL;

// compartment select message timeout
#define TEMP_MESSAGE_TIMEOUT_MS 2000

// OTA
static ghota_client_handle_t *ghota_handle = NULL;

// --- Biến cho tính năng nhấn giữ combo '*' và '#' ---
static bool is_star_pressed = false; // Theo dõi trạng thái nhấn của phím '*'
static bool is_hash_pressed = false; // Theo dõi trạng thái nhấn của phím '#'
static TimerHandle_t combo_hold_timer = NULL; // Timer đếm 5s nhấn giữ đồng thời
#define COMBO_HOLD_TIMEOUT_MS 6000 // 6 giây

#define MAX_WIFI_RETRIES 5
#define WIFI_RETRY_DELAY_MS 8000 // Chờ 8 giây trước khi thử lại
static int wifi_retry_count = 0;

// --- Định nghĩa các ô tủ ---
compartment_t compartments[NUM_COMPARTMENTS] = {
    {'A', OK_STATE} // Giả định OK_STATE, FULL_STATE, FAULT_STATE được định nghĩa
    ,{'B', FULL_STATE}
    ,{'C', FAULT_STATE}
    ,{'D', OK_STATE}
};

// --- Khai báo hàm ---
static void initialize_app(void);
void state_machine_task(void *pvParameter);
void reset_idle_timer(void);
void idle_timer_callback(TimerHandle_t xTimer);
void combo_hold_timer_callback(TimerHandle_t xTimer);
void update_display_state(box_state_t state_to_display);
static void clear_input_buffers(void);
static void time_sync_sntp(void);
static TaskHandle_t prov_request_task_handle = NULL;// Task handle for provisioning request task
static void provisioning_request_task(void *pvParameter);
static void validate_phone_number(void);

#define FIREBASE_WEB_API_KEY_MAIN "AIzaSyDTtRXLnsYXLARXk_LBtd47Aegimp3D9XI" // << THAY THẾ KEY CỦA BẠN
#define FIREBASE_DATABASE_URL_MAIN "https://sample2us-8f23b-default-rtdb.firebaseio.com" // << THAY THẾ URL CỦA BẠN
#define TEST_RTDB_PATH "/testNode" // Ví dụ path


static void clear_input_buffers() {
    memset(phone_number, 0, sizeof(phone_number));
    phone_length = 0;
}

//==========================================================================
// Hàm đồng bộ thời gian với SNTP
//==========================================================================
static void time_sync_sntp(void) {
// Initialize SNTP with configuration
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.sync_cb = NULL;  // we'll poll status manually
        esp_netif_sntp_init(&config);

        // Wait for time to be set
        time_t now = 0;
        struct tm timeinfo = { 0 };
        int retry = 0;
        const int retry_count = 15;
        
        while (timeinfo.tm_year < (2023 - 1900) && ++retry < retry_count) {
            ESP_LOGI("SNTP", "Waiting for system time to be set... (%d/%d)", retry, retry_count);
            vTaskDelay(pdMS_TO_TICKS(3000));
            time(&now);
            localtime_r(&now, &timeinfo);
        }

        if (timeinfo.tm_year < (2023 - 1900)) {
            ESP_LOGE("SNTP", "Failed to sync time.");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }

        ESP_LOGI("SNTP", "Time is synchronized");

    // Set timezone to ICT (Indochina Time, UTC+7)
    setenv("TZ", "ICT-7", 1);
    tzset();

}


//==========================================================================
// Task State Machine Chính
//==========================================================================
void state_machine_task(void *pvParameter) {
    ESP_LOGI(TAG_MAIN, "State Machine Task started. Current state: %d. Waiting for events...", current_state);
    //update_display_state(current_state);
    system_event_t* received_event_ptr = NULL; // Con trỏ để nhận event từ queue
    current_state = BOX_STATE_WIFI_CONNECTING; // Bắt đầu ở trạng thái chờ kết nối WiFi    

    while (1) {
        // Chờ nhận sự kiện từ queue (block vô hạn nếu không có sự kiện)
        if (xQueueReceive(master_event_queue, &received_event_ptr, portMAX_DELAY) == pdTRUE) {
            
            ESP_LOGI(TAG_MAIN, "SM Task: xQueueReceive returned pdTRUE. Event ptr: %p", received_event_ptr); // LOG QUAN TRỌNG

            if (received_event_ptr == NULL) {
                ESP_LOGE(TAG_MAIN, "SM Task received NULL event pointer from queue! Skipping event.");
                continue; // Bỏ qua event không hợp lệ
            }
            ESP_LOGW(TAG_MAIN, "SM Task received Event Type: %d", received_event_ptr->type);
            ESP_LOGI(TAG_MAIN, "Current state: %d", current_state);
            box_state_t previous_state = current_state;

            if (current_state != BOX_STATE_WIFI_PROVISIONING) { // reset idle timer chỉ khi không ở chế độ provisioning
                reset_idle_timer();
            }

            // Xử lý dựa trên loại sự kiện chính
            switch (received_event_ptr->type) {
                
                
                case EVENT_TYPE_SINGLE_LOCK_RESULT:
                    ESP_LOGI(TAG_MAIN,"SINGLE LOCK COMMAND SUCCESS");
                    
                break;

                case EVENT_TYPE_DOOR_STATUS_CHANGED_OPEN:
                    ESP_LOGI(TAG_MAIN,"DOOR OPENED");
                    
                break;
                
                case EVENT_TYPE_DOOR_STATUS_CHANGED_CLOSE:
                    ESP_LOGI(TAG_MAIN,"DOOR CLOSED");
                    
                break;
                
                case EVENT_TYPE_PHONE_VALIDATE_REQUESTED:
                    ESP_LOGI(TAG_MAIN,"HERE");
                    //Không làm gì cả.
                break;
                case EVENT_TYPE_KEYPAD:
                { // Tạo scope để khai báo biến cục bộ
                    keypad_event_data_t kp_data = received_event_ptr->data.keypad;; //Lấy dữ liệu sự kiện keypad
                    bool combo_state_might_have_changed = false; // Cờ kiểm tra xem có cần cập nhật timer combo không

                    //Cập nhật trạng thái nhấn giữ của '*' và '#' ----
                    if (kp_data.key_char == '*') {
                        if (kp_data.type == KEYPAD_PRESSED) {
                            is_star_pressed = true;
                            combo_state_might_have_changed = true;
                        } else if (kp_data.type == KEYPAD_RELEASED) {
                            is_star_pressed = false;
                            combo_state_might_have_changed = true;
                        }
                    //Bỏ qua HOLD của phím đơn lẻ nếu không cần
                    } else if (kp_data.key_char == '#') {
                         if (kp_data.type == KEYPAD_PRESSED) {
                            is_hash_pressed = true;
                            combo_state_might_have_changed = true;
                        } else if (kp_data.type == KEYPAD_RELEASED) {
                            is_hash_pressed = false;
                            combo_state_might_have_changed = true;
                        }
                    // Bỏ qua HOLD của phím đơn lẻ nếu không cần
                    }

                    // Quản lý Combo Hold Timer ----
                    if (combo_state_might_have_changed && combo_hold_timer != NULL) {
                        if (is_star_pressed && is_hash_pressed) {
                            // Cả hai phím đang được nhấn -> Bắt đầu/Reset timer 5 giây
                            ESP_LOGD(TAG_MAIN, "Combo *+# PRESSED, starting/resetting 5s timer.");
                            // Reset timer về 5 giây và bắt đầu đếm
                                if (xTimerReset(combo_hold_timer, pdMS_TO_TICKS(10)) != pdPASS) {
                                    ESP_LOGE(TAG_MAIN, "Failed to reset combo hold timer");
                                }
                        } else {
                            // Ít nhất một trong hai phím đã nhả -> Dừng timer
                            ESP_LOGD(TAG_MAIN, "Combo *+# RELEASED, stopping 5s timer.");
                                if (xTimerStop(combo_hold_timer, pdMS_TO_TICKS(10)) != pdPASS) {
                                    ESP_LOGE(TAG_MAIN, "Failed to stop combo hold timer");
                                }
                        }
                    }

                    // Xử lý dựa trên trạng thái hiện tại của state machine
                    ESP_LOGI(TAG_MAIN, "Processing Keypad Event: Type=%d, Char='%c'", kp_data.type, kp_data.key_char);
                    switch (current_state) {
                        case BOX_STATE_STANDBY: //OK]
                            ESP_LOGD(TAG_MAIN, "[State: STANDBY] Processing Keypad Event");
                            // Chỉ xử lý khi phím được NHẤN
                            if (kp_data.type == KEYPAD_PRESSED) { // <-- Kiểm tra type con của keypad
                                current_state = BOX_STATE_SELECT_COMPARTMENT; // bấm phím bất kỳ sẽ chuyển trạng thái sang chọn ô tủ
                            } else { // Bỏ qua sự kiện RELEASED hoặc HOLD trong state này
                                ESP_LOGD(TAG_MAIN,"Ignoring RELEASED/HOLD event for key '%c' in STANDBY", kp_data.key_char);
                            }
                            break; // BOX_STATE_STANDBY

                        case BOX_STATE_SELECT_COMPARTMENT: //[OK]
                            if (kp_data.type == KEYPAD_PRESSED) {
                                char key = kp_data.key_char;
                                bool compartment_found = false;
                                for (int i = 0; i < NUM_COMPARTMENTS; i++) {
                                    if (key == compartments[i].name) { // Kiểm tra xem có phải phím chọn ô tủ (A-D) không
                                        compartment_found = true;
                                        ESP_LOGD(TAG_MAIN, "Compartment '%c' selected. State: %d", key, compartments[i].state);
                                        selected_compartment = key; // Lưu ô tủ được chọn

                                        // Kiểm tra trạng thái ô tủ và hiển thị [OK]
                                        if (compartments[i].state == FULL_STATE) {
                                            ESP_LOGD(TAG_MAIN, "Action: Display 'Tu %c da day'", key);
                                            // TODO: Hiển thị thông báo tạm "Tu da day" lên LCD
                                            char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
                                            snprintf(msg_buf, sizeof(msg_buf),"CMD_TEMP_MSG:Tu '%c' da day:Hay chon tu khac", selected_compartment);
                                            ST7920_SendToQueue(msg_buf);
                                            // Wait for timeout then return to compartment display
                                            vTaskDelay(pdMS_TO_TICKS(TEMP_MESSAGE_TIMEOUT_MS));
                                            display_compartments(compartments, NUM_COMPARTMENTS);
                                            current_state = BOX_STATE_SELECT_COMPARTMENT;

                                        } else if (compartments[i].state == FAULT_STATE) {
                                            ESP_LOGI(TAG_MAIN, "Action: Display 'Tu %c bi loi'", key);
                                            // TODO: Hiển thị thông báo "Tu bi loi"
                                            char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
                                            snprintf(msg_buf, sizeof(msg_buf),"CMD_TEMP_MSG:Tu '%c' bi loi:Hay chon tu khac", selected_compartment);
                                            ST7920_SendToQueue(msg_buf);
                                            // Wait for timeout then return to compartment display
                                            vTaskDelay(pdMS_TO_TICKS(TEMP_MESSAGE_TIMEOUT_MS));
                                            display_compartments(compartments, NUM_COMPARTMENTS);
                                            current_state = BOX_STATE_SELECT_COMPARTMENT;

                                        } else if (compartments[i].state == OK_STATE) {
                                            phone_length = 0; // Reset bộ đệm SĐT
                                            memset(phone_number, 0, sizeof(phone_number));

                                            //Hiển thị thông báo yêu cầu nhập SĐT
                                            char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
                                            snprintf(msg_buf, sizeof(msg_buf),"CMD_TITLE:Nhap SDT nguoi nhan:SDT hop le co 10 so");
                                            ST7920_SendToQueue(msg_buf);
                                            current_state = BOX_STATE_INPUT_PHONE_NUM; // Chuyển trạng thái
                                        }
                                        break; // Thoát vòng lặp for khi tìm thấy ô tủ
                                    } // end if key == compartment name
                                } // end for compartments

                                if (!compartment_found && (key != '*' && key != '#')) { // Bỏ qua * và # trong state này
                                    ESP_LOGI(TAG_MAIN, "Key '%c' is not a compartment selection key.", key);
                                    // Có thể phát tiếng bíp hoặc không làm gì cả
                                }

                            }  
                            break; // BOX_STATE_SELECT_COMPARTMENT

                        case BOX_STATE_INPUT_PHONE_NUM: //[OK]
                            ESP_LOGD(TAG_MAIN, "[State: INPUT_PHONE_NUM] Processing Keypad Event");
                            if (kp_data.type == KEYPAD_PRESSED) {
                                char key = kp_data.key_char;
                                bool phone_updated = false;

                                if (key >= '0' && key <= '9' && phone_length < (PHONE_MAX_LENGTH - 1)) {    //Nếu bấm phím số từ 0-9 và chưa đủ 10 số
                                    phone_number[phone_length++] = key;
                                    phone_number[phone_length] = '\0';
                                    phone_updated = true;
                                } else if (key == '*') {
                                    if (phone_length > 0) {
                                        phone_number[--phone_length] = '\0';
                                        phone_updated = true;
                                    }
                                } else if (key == '#') {
                                    if (phone_length == 10) {
                                        ESP_LOGD(TAG_MAIN, "Action: Validate phone '%s'. Transitioning to VALIDATING_PHONE_NUM", phone_number);
                                        
                                        //gọi hàm xác thực SĐT
                                        validate_phone_number();// Hàm task xác thực SĐT
                                        
                                    } else {
                                        ESP_LOGW(TAG_MAIN, "Action: Send display cmd 'SDT khong hop le'");
                                        ST7920_SendToQueue("CMD_TEMP_MSG:SDT khong hop le::"); // Ví dụ lệnh lỗi tạm thời
                                        vTaskDelay(pdMS_TO_TICKS(TEMP_MESSAGE_TIMEOUT_MS)); // Chờ 2 giây để hiển thị thông báo
                                        current_state = BOX_STATE_INPUT_PHONE_NUM; // Quay lại trạng thái nhập SĐT
                                    }

                                }
                                // Nếu SĐT thay đổi, gửi lệnh cập nhật display
                                if (phone_updated) {
                                    char buffer[DISPLAY_MESSAGE_MAX_LEN];
                                    snprintf(buffer, sizeof(buffer), "CMD_PHONE:%s", phone_number);
                                    ST7920_SendToQueue(buffer);
                                    ESP_LOGI(TAG_MAIN, "Action: Sent display update for Phone: %s", phone_number);
                                }
                            } else{
                                // Bỏ qua sự kiện RELEASED hoặc HOLD trong state này
                                ESP_LOGD(TAG_MAIN,"Ignoring RELEASED/HOLD event for key '%c' in INPUT_PHONE_NUM", kp_data.key_char);
                            }
                            
                            break; // BOX_STATE_INPUT_PHONE_NUM

                        case BOX_STATE_VALIDATING_PHONE_NUM:
                            if (kp_data.type == KEYPAD_PRESSED && kp_data.key_char == '*') {
                                ESP_LOGI(TAG_MAIN, "Action: Cancel validation. Transitioning back to Select Compartment");
                                current_state = BOX_STATE_SELECT_COMPARTMENT;
                            }
                            break; // BOX_STATE_VALIDATING_PHONE_NUM

                        case BOX_STATE_WIFI_PROVISIONING:
                            if (kp_data.type == KEYPAD_PRESSED && kp_data.key_char == '*') {
                                ESP_LOGI(TAG_MAIN, "Action: Cancel provisioning by keypress. Transitioning back to Select Compartment");
                                stop_wifi_provisioning();
                                current_state = BOX_STATE_SELECT_COMPARTMENT;
                            }
                            // Bỏ qua các keypad event khác
                            break; // BOX_STATE_WIFI_PROVISIONING

                        default:
                            ESP_LOGW(TAG_MAIN, "Keypad event ignored in state %d", current_state);
                            break;
                    } // end switch(current_state) for KEYPAD event
                    break; // EVENT_TYPE_KEYPAD
                } // end case EVENT_TYPE_KEYPAD

            /*case EVENT_TYPE_COMPARTMENT_OPENED: // Nhận được sự kiện ô tủ đã mở - NHÁP
                {
                    compartment_t* opened_compartment = &compartments[selected_compartment - 'A']; // Giả định 'A' là ô tủ đầu tiên
                    ESP_LOGI(TAG_MAIN, "Compartment '%c' opened successfully.", opened_compartment->name);
                    // Hiển thị thông báo thành công
                    char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
                    snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:Tu '%c' da mo thanh cong", opened_compartment->name);
                    ST7920_SendToQueue(msg_buf);
                    vTaskDelay(pdMS_TO_TICKS(TEMP_MESSAGE_TIMEOUT_MS)); // Chờ 2 giây để hiển thị thông báo

                    // Quay về trạng thái chờ
                    current_state = BOX_STATE_STANDBY;
                    display_compartments(compartments, NUM_COMPARTMENTS); // Cập nhật hiển thị ô tủ
                }
                break;
            */
            //=====================================================================
            //CASE xử lý WF       

            case EVENT_TYPE_WIFI_CONNECTED:
                ESP_LOGI(TAG_MAIN, "WiFi Connected event received by SM.");
                    wifi_retry_count = 0;
                    char buffer[DISPLAY_MESSAGE_MAX_LEN];
                    snprintf(buffer, sizeof(buffer), "CMD_WF:DANG CAP NHAT:THOI GIAN HE THONG");
                    ST7920_SendToQueue(buffer);
                    time_sync_sntp();
                ESP_LOGI(TAG_MAIN, "WiFi Connected and Time Synced. Attempting device authentication (Step 1: NVS API Key & Anon Auth)...");
                
                
                esp_err_t auth_init_err = rtdb_client_authenticate_device(FIREBASE_WEB_API_KEY_MAIN); // Gọi hàm mới, hàm này sẽ tự động lấy API key từ NVS
                    if (auth_init_err != ESP_OK) {
                        ESP_LOGE(TAG_MAIN, "Failed to start device authentication process: %s", esp_err_to_name(auth_init_err));
                        current_state = BOX_STATE_FAULT;
                        ST7920_SendToQueue("CMD_SHOW_FAULT_SCREEN"); // Cập nhật hiển thị lỗi
                    } else {
                        ESP_LOGI(TAG_MAIN, "Device authentication process (Step 1) initiated.");
                        current_state = BOX_STATE_STANDBY; // Chuyển về trạng thái chờ sau khi khởi tạo
                        // Gửi lệnh cập nhật hiển thị
                        ST7920_SendToQueue("CMD_SHOW_STANDBY_SCREEN"); // Cập nhật hiển thị về trạng thái chờ
                        // Chờ event EVENT_TYPE_FIREBASE_AUTH_RESULT
                    }

                //ESP_LOGI(TAG_MAIN, "Performing HTTP test after WiFi & SNTP.");
                //    rtdb_client_perform_http_test("https://www.howsmyssl.com/a/check");
                //    if (ghota_handle) 
                //       { ghota_start_update_task(ghota_handle); }
                //ESP_LOGI(TAG_MAIN, "WiFi Connected processing finished. Current state before transition: %d", current_state);
                //    if (current_state != BOX_STATE_FAULT) { // Chỉ chuyển nếu không có lỗi từ trước
                //        current_state = BOX_STATE_STANDBY;
                //    }
                break;

            /*case EVENT_TYPE_HTTP_TEST_RESULT:
                    {
                        rtdb_http_test_result_data_t* http_res = &received_event_ptr->data.http_test_result;
                        ESP_LOGI(TAG_MAIN, "Received HTTP Test Result. Status: %d, HTTP Code: %d", http_res->status, http_res->http_status_code);
                        if (http_res->status == ESP_OK && http_res->http_status_code == 200) {
                            ESP_LOGI(TAG_MAIN, "HTTP Test SUCCEEDED! Response: %.100s...", http_res->response_data);
                            ESP_LOGI(TAG_MAIN, "Requesting Firebase Anonymous Authentication...");
                            rtdb_client_firebase_anonymous_auth(FIREBASE_WEB_API_KEY_MAIN);
                        } else { ESP_LOGE(TAG_MAIN, "HTTP Test FAILED!"); current_state = BOX_STATE_FAULT; }
                    }
                break;*/
            case EVENT_TYPE_RTDB_GET_RESULT:
                    {
                        rtdb_get_result_data_t* get_res = &received_event_ptr->data.rtdb_get_result;
                        ESP_LOGI(TAG_MAIN, "Received RTDB GET Result. Path: %s, Status: %d, HTTP Code: %d",
                                 get_res->path, get_res->status, get_res->http_status_code);
                        if (get_res->status == ESP_OK && get_res->http_status_code == 200) {
                            ESP_LOGI(TAG_MAIN, "RTDB GET SUCCEEDED! Response Preview: %s", get_res->response_preview);
                            // TODO: Parse JSON từ get_res->response_preview
                            // in log response
                            ESP_LOGI(TAG_MAIN, "RTDB GET Response: %s", get_res->response_preview);

                            //Tách chuỗi JSON để lấy SĐT
                            // response_preview là một chuỗi JSON: {"uKey": "12345", "uPNumber": "0366150632"}
                            cJSON *json_response = cJSON_Parse(get_res->response_preview);
                            if (json_response == NULL) {
                                ESP_LOGE(TAG_MAIN, "Failed to parse JSON response: %s", cJSON_GetErrorPtr());
                                ST7920_SendToQueue("CMD_TEMP_MSG:Khong the xac thuc SDT:Vui long thu lai");
                                current_state = BOX_STATE_SELECT_COMPARTMENT; // Quay về trạng thái chờ
                                break;  //Kết thúc case nếu không parse được JSON
                            }
                            // Lấy giá trị của "uPNumber"
                            cJSON *phone_json = cJSON_GetObjectItemCaseSensitive(json_response, "uPNumber");
                            if (cJSON_IsString(phone_json) && (phone_json->valuestring != NULL)) {
                                // Lưu SĐT vào biến cục bộ
                                char response_phone_number[PHONE_MAX_LENGTH] = {0}; // Biến tạm để lưu SĐT

                                strncpy(response_phone_number, phone_json->valuestring, PHONE_MAX_LENGTH - 1);
                                response_phone_number[PHONE_MAX_LENGTH - 1] = '\0'; // Đảm bảo kết thúc chuỗi
                            
                                ESP_LOGI(TAG_MAIN, "Response Phone number verified: %s", response_phone_number);

                                // Hiển thị thông báo xác thực thành công
                                char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
                                snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:SDT '%s' da duoc xac thuc", response_phone_number);
                                ST7920_SendToQueue(msg_buf);

                                //So sánh số điện thoại
                                if(strcmp(phone_number, response_phone_number) == 0) {
                                    ESP_LOGI(TAG_MAIN, "Phone number matches: %s", phone_number);
                                    
                                    //xóa SĐT đã nhập để tránh nhầm lẫn
                                    clear_input_buffers();
                                    char buffer[DISPLAY_MESSAGE_MAX_LEN];
                                    snprintf(buffer, sizeof(buffer), "CMD_PHONE:%s", phone_number);
                                    ST7920_SendToQueue(buffer);
                                    
                                    ESP_LOGI(TAG_MAIN, "Action: Open compartment '%c'", selected_compartment);
                                    //TODO: Gửi lệnh mở ô tủ tương ứng
                                    received_event_ptr->data.lock_channel_to_open = selected_compartment - 'A' + 1; // Giả định kênh khóa tương ứng với ô tủ (A=1, B=2, C=3, D=3)
                                    ESP_LOGI(TAG_MAIN, "Lock channel to open: %d", received_event_ptr->data.lock_channel_to_open);

                                    ESP_LOGI(TAG_MAIN, "EVENT_TYPE_LOCK_COMMAND_TRIGGER: Triggered to open lock channel %d", received_event_ptr->data.lock_channel_to_open);
                                    
                                    esp_err_t lock_cmd_err = lock_controller_open_single_lock(0x01, received_event_ptr->data.lock_channel_to_open); // Giả định 0x01 là địa chỉ bo mạch controller, và lock_channel_to_open là kênh cần mở
                                    ESP_LOGI(TAG_MAIN, "Lock open command sent");
                                    


                                    
                                    } else { 
                                    ESP_LOGW(TAG_MAIN, "Phone number validation WRONG");
                                    }

                                // Chuyển sang trạng thái tiếp theo
                                current_state = BOX_STATE_OPENING_COMPARTMENT; //chuyển sang trạng thái mở ô tủ
                            }
                            // Giải phóng bộ nhớ JSON
                            cJSON_Delete(json_response);
                            
                        } else { ESP_LOGE(TAG_MAIN, "RTDB GET FAILED! Preview: %s", get_res->response_preview); }
                        current_state = BOX_STATE_STANDBY; // Ví dụ, quay về standby

                    }
                break;
                            
            case EVENT_TYPE_FIREBASE_AUTH_RESULT:
                    {
                        rtdb_firebase_auth_result_data_t* auth_res = &received_event_ptr->data.firebase_auth_result;
                        ESP_LOGI(TAG_MAIN, "Received Firebase Auth Result. Status: %d, HTTP Code: %d", auth_res->status, auth_res->http_status_code);
                        
                        char local_id_token[MAX_ID_TOKEN_LEN_INTERNAL] = {0};
                        char local_refresh_token[MAX_REFRESH_TOKEN_LEN_INTERNAL] = {0};
                        char local_uid[MAX_UID_LEN_INTERNAL] = {0};
                        //bool parse_success = false;

                        
                        if (auth_res->status == ESP_OK && auth_res->http_status_code == 200) {
                            ESP_LOGI(TAG_MAIN, "Firebase Auth SUCCEEDED! UID: %s", auth_res->uid);
                            ESP_LOGI(TAG_MAIN, "ID Token: %.20s...", auth_res->id_token);
                            ESP_LOGI(TAG_MAIN, "Refresh Token: %.20s...", auth_res->refresh_token);
                            
                            if (auth_res->parse_success) {
                                ESP_LOGI(TAG_MAIN, "Firebase Auth parse successful!");
                                // Xử lý tiếp các thông tin đã parse
                                ESP_LOGI(TAG_MAIN, "Auth/Refresh successful. UID: %s. Saved tokens to NVS by rtdb_client", auth_res->uid);
                                
                                /*
                                // Đọc lại các giá trị từ NVS để kiểm tra - DEBUG ONLY
                                    nvs_handle_t nvs_handle;
                                    esp_err_t nvs_open_err = nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &nvs_handle);

                                    ESP_LOGI(TAG_MAIN, "Reading back values from NVS for verification...");
                                    char id_token_buf[MAX_ID_TOKEN_LEN_INTERNAL]; // Sử dụng define từ rtdb_client.h
                                    esp_err_t nvs_get_id_token_err = _nvs_read_string(nvs_handle, NVS_KEY_ID_TOKEN, id_token_buf, MAX_ID_TOKEN_LEN_INTERNAL);
                                        if (nvs_get_id_token_err == ESP_OK) {
                                            ESP_LOGI(TAG_MAIN, "ID Token read from NVS: %s", id_token_buf);
                                        } else {
                                            ESP_LOGE(TAG_MAIN, "Failed to read ID Token from NVS: %s", esp_err_to_name(nvs_get_id_token_err));
                                        }
                                    char refresh_token_buf[MAX_REFRESH_TOKEN_LEN_INTERNAL]; // Sử dụng define từ rtdb_client.h
                                    esp_err_t nvs_get_refresh_token_err = _nvs_read_string(nvs_handle, NVS_KEY_REFRESH_TOKEN, refresh_token_buf, MAX_REFRESH_TOKEN_LEN_INTERNAL);
                                        if (nvs_get_refresh_token_err == ESP_OK) {
                                            ESP_LOGI(TAG_MAIN, "Refresh Token read from NVS: %s", refresh_token_buf);
                                        } else {
                                            ESP_LOGE(TAG_MAIN, "Failed to read Refresh Token from NVS: %s", esp_err_to_name(nvs_get_refresh_token_err));
                                        }
                                    char uid_buf[MAX_UID_LEN_INTERNAL];
                                    esp_err_t nvs_get_uid_err = _nvs_read_string(nvs_handle, NVS_KEY_UID, uid_buf, MAX_UID_LEN_INTERNAL);
                                        if (nvs_get_uid_err == ESP_OK) {
                                            ESP_LOGI(TAG_MAIN, "UID read from NVS: %s", uid_buf);
                                        } else {
                                            ESP_LOGE(TAG_MAIN, "Failed to read UID from NVS: %s", esp_err_to_name(nvs_get_uid_err));
                                        }
                                    char web_api_key_buf[MAX_API_KEY_LEN_INTERNAL]; // Sử dụng define từ rtdb_client.h
                                    esp_err_t nvs_get_key_err = _nvs_read_string(nvs_handle, NVS_KEY_API_KEY, web_api_key_buf, MAX_API_KEY_LEN_INTERNAL);
                                        if (nvs_get_key_err == ESP_OK) {
                                            ESP_LOGI(TAG_MAIN, "Web API Key read from NVS: %s", web_api_key_buf);
                                        } else {
                                            ESP_LOGE(TAG_MAIN, "Failed to read Web API Key from NVS: %s", esp_err_to_name(nvs_get_key_err));
                                        }
                                    //DEUG ONLY

                                        // Đóng NVS
                                    nvs_close(nvs_handle);*/

                            } else {
                                ESP_LOGW(TAG_MAIN, "Firebase Auth parse failed or incomplete");
                                // Xử lý lỗi parse
                            }

                            // Yêu cầu đọc dữ liệu từ RTDB
                            //ESP_LOGI(TAG_MAIN, "Requesting data from RTDB path: %s", TEST_RTDB_PATH);
                            //rtdb_client_get_data(FIREBASE_DATABASE_URL_MAIN, TEST_RTDB_PATH);
                        
                        } else {
                            ESP_LOGE(TAG_MAIN, "Firebase Auth FAILED! Error: %s", auth_res->error_message);

                            //Test Fallback (NVS có Refresh Token không hợp lệ):
                            //Xác nhận yêu cầu làm mới token thất bại (ví dụ HTTP 400).
                        /*    if (auth_res->status == ESP_FAIL && auth_res->http_status_code == 400) {
                                ESP_LOGI(TAG_MAIN, "Refresh Token expired or invalid. Starting provisioning.");
                                ESP_LOGW(TAG_MAIN, "Token refresh failed (HTTP %d). Clearing NVS tokens and attempting new anonymous sign-in.", auth_res->http_status_code);

                                nvs_handle_t nvs_h_fallback;
                                if (nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &nvs_h_fallback) == ESP_OK) {
                                    nvs_erase_key(nvs_h_fallback, NVS_KEY_ID_TOKEN);
                                    nvs_erase_key(nvs_h_fallback, NVS_KEY_REFRESH_TOKEN);
                                    nvs_erase_key(nvs_h_fallback, NVS_KEY_UID); // UID gắn với session ẩn danh cũ, nên cũng xóa
                                    nvs_commit(nvs_h_fallback);
                                    nvs_close(nvs_h_fallback);
                                    ESP_LOGI(TAG_MAIN, "Invalid/Expired tokens cleared from NVS.");
                                }
                                ESP_LOGI(TAG_MAIN, "Xóa token trong RAM");
                                // Xóa token trong RAM
                                //memset(s_id_token, 0, sizeof(s_id_token)); // Xóa id_token RAM
                                //memset(s_refresh_token, 0, sizeof(s_refresh_token));
                                //memset(s_uid, 0, sizeof(s_uid));
                            } else { // Lỗi khác
                                current_state = BOX_STATE_FAULT;
                                update_display_state(current_state); // Cập nhật hiển thị lỗi
                            }*/
                        }
                    }
                break;
                            // TODO: Thêm case cho PUT, PATCH, DELETE results
                // case EVENT_TYPE_RTDB_PUT_RESULT:
                // {
                //     rtdb_write_result_data_t* put_res = &received_event_ptr->data.rtdb_write_result;
                //     ESP_LOGI(TAG_MAIN, "Received RTDB PUT Result. Path: %s, Status: %d, HTTP Code: %d",
                //              put_res->path, put_res->status, put_res->http_status_code);
                //     if (put_res->status == ESP_OK && put_res->http_status_code == 200) {
                //         ESP_LOGI(TAG_MAIN, "RTDB PUT SUCCEEDED!");
                //     } else { ESP_LOGE(TAG_MAIN, "RTDB PUT FAILED!"); }
                //     current_state = BOX_STATE_STANDBY;
                // }
                // break;

            case EVENT_TYPE_WIFI_CONNECT_FAIL: //[OK]
                {
                wifi_retry_count++; // Tăng bộ đếm
                ESP_LOGW(TAG_MAIN, "WiFi connection failed. Attempt %d/%d...", wifi_retry_count, MAX_WIFI_RETRIES);

                if (wifi_retry_count <= MAX_WIFI_RETRIES) {
                    //Thử kết nối lại !!!
                    char retry_msg[40];
                    snprintf(retry_msg, sizeof(retry_msg), "CMD WF: WiFi Retry: %d : %d", wifi_retry_count, MAX_WIFI_RETRIES);
                    ST7920_SendToQueue(retry_msg); // Hiển thị trạng thái thử lại

                    ESP_LOGI(TAG_MAIN, "Waiting %d ms before retry...", WIFI_RETRY_DELAY_MS);
                    vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS)); // Đợi trước khi thử lại

                    ESP_LOGI(TAG_MAIN, "Retrying WiFi connection...");
                    esp_err_t retry_err = wifi_manager_connect_sta(); //
                    if (retry_err != ESP_OK) {
                        ESP_LOGE(TAG_MAIN, "Failed to start WiFi connection retry attempt.");
                        // Nếu ngay cả việc bắt đầu thử lại cũng lỗi -> vào provisioning luôn?
                        wifi_retry_count = 0; // Reset counter
                        esp_err_t prov_err = start_wifi_provisioning(); //
                        if (prov_err == ESP_OK) {
                            current_state = BOX_STATE_WIFI_PROVISIONING; //
                            clear_input_buffers();
                            ST7920_SendToQueue("CMD_SHOW_PROV_SCREEN"); // Gửi lệnh hiển thị provisioning
                        } else { /* Xử lý lỗi */ current_state = BOX_STATE_FAULT; ST7920_SendToQueue("CMD_SHOW_FAULT_SCREEN");}

                    }
                 // Vẫn ở trạng thái BOX_STATE_WIFI_CONNECTING để chờ kết quả lần thử mới
                } else {
                 // !!!Đã hết số lần thử -> vào provisioning !!!
                 ESP_LOGE(TAG_MAIN, "Max WiFi retries reached. Starting provisioning.");
                 wifi_retry_count = 0; // Reset bộ đếm cho lần sau
                 esp_err_t prov_err = start_wifi_provisioning(); //
                    if (prov_err == ESP_OK) {
                        current_state = BOX_STATE_WIFI_PROVISIONING; //
                        ESP_LOGI(TAG_MAIN, "Start PROV after retries");
                        clear_input_buffers();
                        ST7920_SendToQueue("CMD_SHOW_PROV_SCREEN");
                    } else {
                        ESP_LOGE(TAG_MAIN, "Failed to start provisioning after max retries: %s", esp_err_to_name(prov_err));
                        current_state = BOX_STATE_FAULT;
                        ST7920_SendToQueue("CMD_SHOW_FAULT_SCREEN");
                    }
                }
                } // end EVENT_TYPE_WIFI_CONNECT_FAIL
                break;
            
            case EVENT_TYPE_WIFI_DISCONNECTED:
                    ESP_LOGW(TAG_MAIN, "WiFi disconnected. Attempting to reconnect or go to provisioning.");
                    current_state = BOX_STATE_WIFI_CONNECTING; // Thử kết nối lại
                    char wf_disp_buffer[DISPLAY_MESSAGE_MAX_LEN];
                    snprintf(wf_disp_buffer, sizeof(wf_disp_buffer), "CMD WF: Dang ket noi lai: %d/%d", wifi_retry_count, MAX_WIFI_RETRIES);
                    ST7920_SendToQueue(wf_disp_buffer); // Hiển thị trạng thái kết nối lại
                    wifi_manager_connect_sta(); // Gọi hàm kết nối lại từ wifi_manager
                    break;

            case EVENT_TYPE_PROVISIONING_REQUEST:   //[OK]
                    ESP_LOGI(TAG_MAIN, "Provisioning Request Event Received");
                    
                    if (current_state != BOX_STATE_WIFI_PROVISIONING) {// Luôn cho phép vào provisioning từ bất kỳ state nào trừ chính nó
                         ESP_LOGI(TAG_MAIN, "Action: Starting WiFi Provisioning. Transitioning to WIFI_PROVISIONING");
                         box_state_t previous_prov_state = current_state; // Lưu state cũ để quay lại nếu lỗi
                         current_state = BOX_STATE_WIFI_PROVISIONING;

                         if(idle_timer != NULL) xTimerStop(idle_timer, 0);

                         esp_err_t prov_err = start_wifi_provisioning();
                         if (prov_err == ESP_OK) {
                            wifi_retry_count = 0; // Reset retry khi vào provisioning thủ công
                             // Gửi lệnh vẽ màn hình provisioning MỘT LẦN
                             ESP_LOGI(TAG_MAIN, "Action: Send display cmd SHOW_PROV_SCREEN");
                             ST7920_SendToQueue("CMD_SHOW_PROV_SCREEN"); // Cần định nghĩa lệnh này trong display_task
                         } else {
                             ESP_LOGE(TAG_MAIN, "Failed to start provisioning: %s. Reverting state.", esp_err_to_name(prov_err));
                             current_state = previous_prov_state; // Quay lại state trước đó
                             ST7920_SendToQueue("CMD_TEMP_MSG:Khong the vao WF Setup::"); // Thông báo lỗi
                             if (current_state != BOX_STATE_STANDBY) { // Chỉ reset idle nếu không phải standby
                                 reset_idle_timer();
                             }
                         }
                    } else {
                         ESP_LOGW(TAG_MAIN,"Already in provisioning state.");
                    }
                    break; // EVENT_TYPE_PROVISIONING_REQUEST

            case EVENT_TYPE_WIFI_PROVISION:
                {
                        wifi_prov_event_data_t wifi_evt = received_event_ptr->data.wifi;
                         ESP_LOGI(TAG_MAIN, "WiFi Provisioning Event Status Received: %d", wifi_evt.status);

                        if (current_state == BOX_STATE_WIFI_PROVISIONING) {
                            if (wifi_evt.status == WIFI_PROVISION_SUCCESS) {
                                ESP_LOGI(TAG_MAIN, "Provisioning SUCCESS. Transitioning to STANDBY");
                                current_state = BOX_STATE_STANDBY;
                                ESP_LOGI(TAG_MAIN, "Action: Send display cmd PROV_SUCCESS");
                                // Gửi lệnh hiển thị thành công (có thể kèm SSID)
                                char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
                                snprintf(msg_buf, sizeof(msg_buf),"CMD_TEMP_MSG:WiFi Da Ket Noi:%s", wifi_evt.ssid); // Ví dụ
                                ST7920_SendToQueue(msg_buf);
                            } else if (wifi_evt.status == WIFI_PROVISION_FAIL) {
                                ESP_LOGE(TAG_MAIN, "Provisioning FAILED. Transitioning back to STANDBY");
                                current_state = BOX_STATE_STANDBY;
                                ESP_LOGI(TAG_MAIN, "Action: Send display cmd PROV_FAIL");
                                ST7920_SendToQueue("CMD_TEMP_MSG:Loi Cai Dat WiFi:Hay thu lai"); // Ví dụ
                                // reset_idle_timer(); // State machine sẽ tự vẽ lại màn hình chờ ở cuối
                            }
                        } else {
                             ESP_LOGW(TAG_MAIN, "Ignoring WiFi Provision event in state %d", current_state);
                        }
                    }
                    break; // EVENT_TYPE_WIFI_PROVISION

            case EVENT_TYPE_TIMER_IDLE:
                    ESP_LOGI(TAG_MAIN, "Idle Timer Expired Event Received"); 
                    if (current_state != BOX_STATE_STANDBY) {
                        ESP_LOGI(TAG_MAIN, "Action: Returning to STANDBY state due to inactivity.");
                        current_state = BOX_STATE_STANDBY;
                        selected_compartment = '\0';
                        phone_length = 0;
                        memset(phone_number, 0, sizeof(phone_number));
                    } else {
                        ESP_LOGI(TAG_MAIN, "Already in STANDBY state. No action taken.");
                    }
                    break; // EVENT_TYPE_TIMER_IDLE
                // Thêm case để xử lý kết quả HTTP Test
                
            default:
                    ESP_LOGW(TAG_MAIN, "Unknown event type received in SM: %d", received_event_ptr->type);
                    break;
            } // end switch(received_event_ptr->type)
            // Giải phóng bộ nhớ cho event đã nhận
            event_pool_release_buffer(received_event_ptr);
            received_event_ptr = NULL; // Quan trọng để tránh use-after-release
            ESP_LOGI(TAG_MAIN, "SM Task: Event processed and buffer released. Current state: %d", current_state);
            // Cập nhật trạng thái hiện tại

            if (previous_state != current_state) {
                ESP_LOGI(TAG_MAIN,"State changed from %d to %d. Updating display.", previous_state, current_state);
                //update_display_state(current_state);
                //tạo biến lấy tên state
                    char state_name[32];
                    sprintf(state_name, "State: %d", current_state);
                    ST7920_SendToQueue(state_name);
                if(current_state != BOX_STATE_STANDBY && current_state != BOX_STATE_WIFI_PROVISIONING && current_state != BOX_STATE_INITIALIZING) {
                    reset_idle_timer();
                    //RESET idle timer mỗi khi nhận được sự kiện mới
                } else if (current_state == BOX_STATE_STANDBY && idle_timer != NULL) {
                    ESP_LOGI(TAG_MAIN, "State is STANDBY. xTimerStop(idle_timer, 0)");
                    xTimerStop(idle_timer, 0);
                }
            }
        } else {
            ESP_LOGW(TAG_MAIN, "SM Task: xQueueReceive returned pdFALSE (should not happen with portMAX_DELAY unless queue deleted).");
        }// end if(QueueReceive)
    } // end while(1)
} // end state_machine_task


//==========================================================================
// Hàm xử lý sự kiện từ esp_ghota
//==========================================================================

static void ghota_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
ESP_LOGI(TAG_MAIN, "GHOTA Event received: base=%s, event_id=%" PRIi32, event_base, event_id);
// Thêm switch case để xử lý các loại sự kiện GHOTA
switch(event_id) {
    case GHOTA_EVENT_START_CHECK:
        ESP_LOGI(TAG_MAIN, "GHOTA: Checking for new release");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_UPDATE_AVAILABLE:
        ESP_LOGI(TAG_MAIN, "GHOTA: New Version Available");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_NOUPDATE_AVAILABLE:
        ESP_LOGI(TAG_MAIN, "GHOTA: No Update Available");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_START_UPDATE:
        ESP_LOGI(TAG_MAIN, "GHOTA: Starting Firmware Update");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_FINISH_UPDATE:
        ESP_LOGI(TAG_MAIN, "GHOTA: Firmware Update Successful");

            char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
            snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:OTA Thanh Cong:Dang khoi dong lai...");
            ST7920_SendToQueue(msg_buf);

        break;
    case GHOTA_EVENT_UPDATE_FAILED:
        ESP_LOGE(TAG_MAIN, "GHOTA: Firmware Update Failed");
        // TODO: Cập nhật display thông báo lỗi
            snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:OTA That Bai:Kiem tra ket noi");
            ST7920_SendToQueue(msg_buf);
        break;
    case GHOTA_EVENT_START_STORAGE_UPDATE:
         ESP_LOGI(TAG_MAIN, "GHOTA: Starting Storage Update");
         break;
    case GHOTA_EVENT_FINISH_STORAGE_UPDATE:
         ESP_LOGI(TAG_MAIN, "GHOTA: Storage Update Successful");
         break;
    case GHOTA_EVENT_STORAGE_UPDATE_FAILED:
         ESP_LOGE(TAG_MAIN, "GHOTA: Storage Update Failed");
         break;
    case GHOTA_EVENT_FIRMWARE_UPDATE_PROGRESS: {
        int progress = *(int*)event_data;
        ESP_LOGV(TAG_MAIN, "GHOTA: Firmware Update Progress: %d%%", progress);
        // TODO: Cập nhật display thanh tiến trình
        // Có thể gửi lệnh dạng "CMD_PROGRESS:firmware:%d"
        char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
        snprintf(msg_buf, sizeof(msg_buf), "CMD_PROGRESS:firmware:%d", progress);
        ST7920_SendToQueue(msg_buf);

        break;
    }
    case GHOTA_EVENT_STORAGE_UPDATE_PROGRESS: {
         int progress = *(int*)event_data;
         ESP_LOGV(TAG_MAIN, "GHOTA: Storage Update Progress: %d%%", progress);
         // TODO: Cập nhật display thanh tiến trình
         // Có thể gửi lệnh dạng "CMD_PROGRESS:storage:%d"
         char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
         snprintf(msg_buf, sizeof(msg_buf), "CMD_PROGRESS:storage:%d", progress);
         ST7920_SendToQueue(msg_buf);
         
         break;
    }
    case GHOTA_EVENT_PENDING_REBOOT:
         ESP_LOGI(TAG_MAIN, "GHOTA: Reboot Pending");
         // TODO: Cập nhật display thông báo khởi động lại

            ST7920_SendToQueue("CMD_TEMP_MSG:Khoi dong lai...");

         break;
    default:
        ESP_LOGW(TAG_MAIN, "Unknown GHOTA Event ID: %" PRIi32, event_id);
        break;
}
}


//==========================================================================
// Hàm để chẩn bị cho việc xác thực số điện thoại
//==========================================================================
void validate_phone_number(void) {
            // Acquire a buffer from the pool
            system_event_t* phone_evt_ptr = event_pool_acquire_buffer(pdMS_TO_TICKS(10)); // Use a small timeout
            // Tạo đường dẫn RTDB để xác thực SĐT (thêm "u" vào trước phone_number để phù hợp với định dạng Firebase RTDB)
            char rtdb_verifyPN_path[64];
            memset(rtdb_verifyPN_path, 0, sizeof(rtdb_verifyPN_path)); // Đảm bảo xóa sạch buffer
            snprintf(rtdb_verifyPN_path, sizeof(rtdb_verifyPN_path), "/uLock/u%s", phone_number);
            ESP_LOGD(TAG_MAIN, "Firebase RTDB path for phone verification: %s", rtdb_verifyPN_path);
            //Gửi SDT đến Firebase RTDB để xác thực
            //ESP_LOGI(TAG_MAIN, "Requesting data from RTDB path: %s", TEST_RTDB_PATH);
            
            esp_err_t rtdb_get_err = rtdb_client_get_data(FIREBASE_DATABASE_URL_MAIN, rtdb_verifyPN_path);

            current_state = BOX_STATE_VALIDATING_PHONE_NUM;
            ST7920_SendToQueue("CMD_NOTICE2:Dang xac thuc SDT...");

            if (rtdb_get_err != ESP_OK) {
                ESP_LOGE(TAG_MAIN, "Failed to get data from RTDB path '%s': %s", rtdb_verifyPN_path, esp_err_to_name(rtdb_get_err));
                ST7920_SendToQueue("CMD_TEMP_MSG:Khong the xac thuc SDT:Vui long thu lai");
                current_state = BOX_STATE_INPUT_PHONE_NUM; // Quay về trạng thái chờ nhập SĐT

            } else {    // Nếu gửi thành công
                ESP_LOGI(TAG_MAIN, "RTDB request sent successfully for phone verification.");
                if (phone_evt_ptr) {
                    // Populate the event data
                    phone_evt_ptr->type = EVENT_TYPE_PHONE_VALIDATE_REQUESTED;
                    phone_evt_ptr->timestamp = (uint32_t)(esp_timer_get_time() / 1000);

                    // Send the pointer to the event buffer to master queue
                    if (master_event_queue != NULL) {
                        if (xQueueSend(master_event_queue, &phone_evt_ptr, pdMS_TO_TICKS(100)) != pdPASS) {
                            ESP_LOGE(TAG_MAIN, "Failed to send phone validation event! Releasing buffer.");
                            event_pool_release_buffer(phone_evt_ptr); // Release if send fails
                        } else {
                            ESP_LOGI(TAG_MAIN, "Phone validation event sent successfully");
                            // Buffer is now owned by master_event_queue; state_machine_task will release it
                        }
                    } else {
                        ESP_LOGE(TAG_MAIN, "Master event queue NULL in validate_phone_number_task. Releasing buffer.");
                        event_pool_release_buffer(phone_evt_ptr); // Should not happen if init is correct
                    }
                } else {
                    ESP_LOGE(TAG_MAIN, "Failed to acquire event buffer for phone validation!");
                }
            }
            
    }// end


//==========================================================================
//Task xử lý RS485
//==========================================================================


void rs485_RX_task(void *pvParameter) {
    uint8_t rx_buffer[RS485_MAX_PACKET_SIZE] = {0}; // Buffer nhận dữ liệu thô, kích thước động
    system_event_t* event_ptr = NULL; // Con trỏ tới event từ pool

    ESP_LOGI(TAG_MAIN, "RS485 RX Task started in continuous listening mode.");

    while (1) { // Bắt đầu vòng lặp nhận dữ liệu từ RS485, task này sẽ liên tục kiểm tra UART.
        
        // Gọi rs485_driver_read_data để đọc một gói tin.
        // Timeout 1500ms cho toàn bộ gói tin, bao gồm cả tìm Start Code.
        int read_bytes = rs485_driver_read_data(s_uart_num, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(1000)); 

        if (read_bytes > 0) { // Có gói tin được nhận
            ESP_LOGI(TAG_MAIN, "Successfully received %d bytes from controller.", read_bytes);

            // BẮT ĐẦU SỬ DỤNG EVENT POOL
            event_ptr = event_pool_acquire_buffer(pdMS_TO_TICKS(100)); // Cố gắng mượn event từ pool, chờ 100ms
            if (event_ptr == NULL) {
                ESP_LOGE(TAG_MAIN, "Failed to acquire event from pool. Dropping RS485 packet.");
                vTaskDelay(pdMS_TO_TICKS(5)); // Nhường CPU một chút nếu pool trống
                continue; // Bỏ qua gói tin này và thử lại trong vòng lặp tiếp theo
            }

            // Gói tin đã được đồng bộ hóa và lưu vào rx_buffer bởi rs485_driver_read_data
            // Bây giờ, hãy phân tích gói tin này và kiểm tra tính hợp lệ.

            // Bước 1: Kiểm tra Start Code (đã được rs485_driver_read_data đảm bảo, nhưng kiểm tra lại cho an toàn)
            if (memcmp(rx_buffer, START_CODE, 4) != 0) {
                ESP_LOGE(TAG_MAIN, "Invalid Start Code in response. Releasing event.");
                event_ptr->type = EVENT_TYPE_RS485_ERROR; // Gán loại lỗi
                event_ptr->data.rs485_error.error_code = RS485_ERR_INVALID_START_CODE;
                // event_ptr->data.rs485_error.board_id/command_word có thể chưa xác định
                xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(100));
                continue; // Bỏ qua gói tin này
            }

            // Bước 2: Lấy Frame Length và kiểm tra khớp với số byte đọc được
            uint8_t frame_length = rx_buffer[4]; 
            if (read_bytes != frame_length) { // read_bytes là số byte thực tế đã đọc từ UART
                ESP_LOGE(TAG_MAIN, "Actual received length (%d) does not match reported Frame Length (%d). Packet might be corrupted or incomplete. Releasing event.", read_bytes, frame_length);
                event_ptr->type = EVENT_TYPE_RS485_ERROR;
                event_ptr->data.rs485_error.error_code = RS45_ERR_FRAME_LENGTH_MISMATCH;
                // board_id và command_word có thể lấy được nếu gói tin đủ dài để đọc các byte này
                if (read_bytes >= 7) { 
                    event_ptr->data.rs485_error.board_id = rx_buffer[5];
                    event_ptr->data.rs485_error.command_word = rx_buffer[6];
                }
                xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(10));
                continue; 
            }

            // Bước 3: Kiểm tra độ dài tối thiểu của gói tin (đã có Start Code, Frame Length, Board ID, Cmd Word, Data (min 1 byte), Checksum)
            if (frame_length < 9) { // Tối thiểu 9 byte
                ESP_LOGE(TAG_MAIN, "Received packet too short (%d bytes) after Frame Length check. Releasing event.", frame_length);
                event_ptr->type = EVENT_TYPE_RS485_ERROR;
                event_ptr->data.rs485_error.error_code = RS485_ERR_INCOMPLETE_FRAME;
                // board_id và command_word có thể lấy được nếu gói tin đủ dài
                if (read_bytes >= 7) { 
                    event_ptr->data.rs485_error.board_id = rx_buffer[5];
                    event_ptr->data.rs485_error.command_word = rx_buffer[6];
                }
                xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(10));
                continue;
            }

            // Lấy các trường cơ bản sau khi các kiểm tra độ dài đã qua
            uint8_t board_id = rx_buffer[5];
            uint8_t command_word = rx_buffer[6];
            
            // Bước 4: Kiểm tra Checksum
            // Checksum được tính từ byte Frame Length (index 4) đến hết Data Field (trước byte Checksum cuối cùng)
            // Độ dài dữ liệu để tính checksum = Frame Length - (Start Code + Checksum) = frame_length - 5
            uint8_t calculated_checksum = lock_controller_calculate_checksum(&rx_buffer[4], frame_length - 5);
            uint8_t received_checksum = rx_buffer[frame_length - 1]; // Byte cuối cùng là Checksum
            
            if (calculated_checksum == received_checksum) {
                ESP_LOGI(TAG_MAIN, "Packet Checksum OK. Board ID: 0x%02X, Command Word: 0x%02X", board_id, command_word);

                // Điền các trường chung của event_ptr
                event_ptr->data.controller_response.frame_length = frame_length;
                event_ptr->data.controller_response.board_id = board_id;
                event_ptr->data.controller_response.command_word = command_word;
                event_ptr->timestamp = (uint32_t)(esp_timer_get_time() / 1000); // Lấy thời gian hiện tại

                // Bước 5: Phân loại gói tin dựa trên Command Word và Frame Length
                switch (command_word) { 
                    case CMD_OPEN_SINGLE_LOCK: { // Phản hồi 0x82 cho lệnh mở 1 cửa (Frame Length = 0x0B)
                        // Phản hồi: 57 4B 4C 59 | 0B | 01 82 | 00 04 00 | 85 (Checksum)
                        // Status: rx_buffer[7], Lock Channel: rx_buffer[8], Lock Status: rx_buffer[9]
                        if (frame_length != 0x0B) {
                            ESP_LOGW(TAG_MAIN, "CMD_OPEN_SINGLE_LOCK (0x%02X) response with unexpected Frame Length %d. Expected 0x0B. Processing anyway.", command_word, frame_length);
                            // Bạn có thể xử lý lỗi ở đây nếu muốn chặt chẽ hơn.
                            // Hiện tại sẽ cố gắng đọc data field theo format 0x0B
                        }
                        event_ptr->type = EVENT_TYPE_SINGLE_LOCK_RESULT;
                        event_ptr->data.controller_response.command_status = rx_buffer[7];
                        event_ptr->data.controller_response.lock_channel = rx_buffer[8]; // Kênh khóa
                        event_ptr->data.controller_response.lock_status = rx_buffer[9]; // Trạng thái khóa (00: thành công, 01: thất bại)

                        // Ghi log chi tiết
                        ESP_LOGI(TAG_MAIN, "Single Lock Command Response Details:");
                        ESP_LOG_BUFFER_HEXDUMP(TAG_MAIN, rx_buffer, read_bytes, ESP_LOG_INFO);
                        break;
                    }

                    case CMD_UPLOAD_DOOR_STATUS_CHANGE: { // 0x85: Báo cáo thay đổi trạng thái cửa (Frame Length = 0x0A)
                        // Phản hồi: 57 4B 4C 59 | 0A | 01 85 | 04 00 | CS (Checksum)
                        // Kênh khóa: rx_buffer[7], Trạng thái cửa: rx_buffer[8]
                        if (frame_length != 0x0A) {
                             ESP_LOGW(TAG_MAIN, "CMD_UPLOAD_DOOR_STATUS_CHANGE (0x%02X) with unexpected Frame Length %d. Expected 0x0A. Processing anyway.", command_word, frame_length);
                        }
                        
                        event_ptr->data.controller_response.lock_channel = rx_buffer[7]; // Kênh khóa
                        uint8_t door_change_status = rx_buffer[8]; // 00: Cửa đóng -> mở, 01: Cửa mở -> đóng

                        switch (door_change_status) {
                            case 0:
                                event_ptr->type = EVENT_TYPE_DOOR_STATUS_CHANGED_OPEN;
                                break;
                            case 1:
                                event_ptr->type = EVENT_TYPE_DOOR_STATUS_CHANGED_CLOSE;
                                break;
                            default:
                                ESP_LOGE(TAG_MAIN, "Abnormal door status: 0x%02X. Releasing event.", door_change_status);
                                event_ptr->type = EVENT_TYPE_RS485_ERROR; // Gán loại lỗi nếu trạng thái không xác định
                                event_ptr->data.rs485_error.error_code = RS485_ERR_INVALID_DATA_LEN;
                                xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(10));
                                event_ptr = NULL; // Đặt NULL để không gửi event lỗi này sau đó
                                break;
                        }
                        
                        if (event_ptr != NULL) { // Chỉ log và gửi nếu event vẫn hợp lệ
                            ESP_LOGI(TAG_MAIN, "Door Status Change (0x%02X) from Channel 0x%02X. Status: 0x%02X", 
                                     command_word, event_ptr->data.controller_response.lock_channel, door_change_status);
                            ESP_LOG_BUFFER_HEXDUMP(TAG_MAIN, rx_buffer, read_bytes, ESP_LOG_INFO);
                        }
                        break;
                    }
                    
                    /*case CMD_OPEN_ALL_LOCKS: { // Phản hồi 0x86 cho lệnh mở tất cả cửa (Frame Length = 0x09)
                        // Phản hồi: 57 4B 4C 59 | 09 | 00 86 | 00 | 86 (Checksum)
                        // Status: rx_buffer[7]
                        if (frame_length != 0x09) {
                            ESP_LOGW(TAG_MAIN, "CMD_OPEN_ALL_LOCKS (0x%02X) response with unexpected Frame Length %d. Expected 0x09. Processing anyway.", command_word, frame_length);
                        }
                        event_ptr->type = EVENT_TYPE_LOCK_OPERATION_RESULT; // Hoặc một loại event khác cho mở tất cả
                        event_ptr->data.controller_response.command_status = rx_buffer[7];
                        ESP_LOGI(TAG_MAIN, "Open All Locks (0x%02X) Response. Status: 0x%02X", command_word, event_ptr->data.controller_response.command_status);
                        ESP_LOG_BUFFER_HEXDUMP(TAG_MAIN, rx_buffer, read_bytes, ESP_LOG_INFO);
                        break;
                    }
                    
                    case CMD_OPEN_MULTIPLE_LOCKS: { // Phản hồi 0x87 cho lệnh mở nhiều khóa (Frame Length > 0x09)
                        // Phản hồi: 57 4B 4C 59 | XX | 00 87 | [Status Kênh N] | CS
                        // Số lượng byte data = frame_length - 8 (4 SC + 1 FL + 1 BID + 1 CW + 1 CS)
                        int num_status_bytes = frame_length - 8;
                        if (num_status_bytes > 0 && num_status_bytes <= (RS485_MAX_PACKET_SIZE - 9) ) { // Đảm bảo đủ chỗ trong buffer
                            event_ptr->type = EVENT_TYPE_MULTI_LOCK_OPERATION_RESULT; // Cần định nghĩa event này
                            // Bạn cần một mảng trong event_ptr->data để lưu các trạng thái
                            // Ví dụ: event_ptr->data.multi_lock_result.num_channels = num_status_bytes;
                            // memcpy(event_ptr->data.multi_lock_result.statuses, &rx_buffer[7], num_status_bytes);
                            ESP_LOGI(TAG_MAIN, "Received 0x87 response with %d status bytes.", num_status_bytes);
                            ESP_LOG_BUFFER_HEXDUMP(TAG_MAIN, rx_buffer, read_bytes, ESP_LOG_INFO);
                        } else {
                            ESP_LOGE(TAG_MAIN, "CMD_OPEN_MULTIPLE_LOCKS (0x%02X) with invalid data length. Frame Length: %d", command_word, frame_length);
                            event_ptr->type = EVENT_TYPE_RS485_ERROR;
                            event_ptr->data.rs485_error.error_code = RS485_ERR_INVALID_DATA_LEN;
                        }
                        break;
                    }*/

                    default:
                        ESP_LOGI(TAG_MAIN, "Unknown Command Word 0x%02X from board %d. Releasing event.", command_word, board_id);
                        event_pool_release_buffer(event_ptr); 
                        event_ptr = NULL; // Đặt NULL để không gửi nếu đã được xử lý hoặc không hợp lệ
                        break;
                }

                if (event_ptr != NULL) { // Chỉ gửi event nếu nó vẫn còn hợp lệ và chưa được release
                    if (xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(100)) != pdPASS) {
                        ESP_LOGE(TAG_MAIN, "Failed to send RS485 event to queue! Releasing buffer.");
                        event_pool_release_buffer(event_ptr); // Release if send fails
                    } else { 
                        ESP_LOGI(TAG_MAIN, "RS485 event sent successfully.");
                        // Buffer is now owned by master_event_queue; state_machine_task will release it
                    }
                } 
            } else { // Checksum mismatch
                ESP_LOGE(TAG_MAIN, "Checksum mismatch in response. Calculated: 0x%02X, Received: 0x%02X. Releasing event.", calculated_checksum, received_checksum);
                event_ptr->type = EVENT_TYPE_RS485_ERROR;
                event_ptr->data.rs485_error.error_code = RS485_ERR_CHECKSUM_MISMATCH;
                event_ptr->data.rs485_error.board_id = board_id;
                event_ptr->data.rs485_error.command_word = command_word;
                xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(10));
            }
        } else if (read_bytes == 0) { // Không có gói tin nào trong timeout
            ESP_LOGD(TAG_MAIN, "No RS485 packet in this cycle. Yielding CPU.");
            vTaskDelay(pdMS_TO_TICKS(5)); // Nhường CPU khi không có dữ liệu
        } else { // read_bytes == -1 (lỗi đọc UART từ driver)
            ESP_LOGE(TAG_MAIN, "Error during RS485 read operation. Will retry after a short delay.");
            vTaskDelay(pdMS_TO_TICKS(20)); // Đợi lâu hơn một chút sau lỗi
        }
    } 
    ESP_LOGI(TAG_MAIN, "RS485 RX Task terminating.");
    vTaskDelete(NULL); // Task sẽ tự xóa khi thoát khỏi vòng lặp (hiếm khi xảy ra)
}



/*void rs485_RX_task(void *pvParameter) {
    uint8_t rx_buffer[11] = {0}; // Max response size
    system_event_t* event_ptr; // Con trỏ tới event từ pool
    ESP_LOGI(TAG_MAIN, "RS485 RX Task startedin continuous listening mode");


    while (1) { // Bắt đầu vòng lặp nhận dữ liệu từ RS485
        //Chờ xTaskNotifyTake từ RS485 driver để bắt đầu nhận dữ liệu
        //xTaskNotifyWait(0, 0, NULL, portMAX_DELAY); // Chờ thông báo từ RS485 driver

    
    int read_bytes = rs485_driver_read_data(s_uart_num, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(1000)); // Timeout 1000ms
    if (read_bytes >= 9) { // Kiểm tra độ dài tối thiểu của một gói tin phản hồi
        ESP_LOGI(TAG_MAIN, "Successfully received %d bytes from controller.", read_bytes);

            // BẮT ĐẦU SỬ DỤNG EVENT POOL
            event_ptr = event_pool_acquire_buffer(pdMS_TO_TICKS(100)); // Cố gắng mượn event từ pool, chờ 100ms
            if (event_ptr == NULL) {
                ESP_LOGE(TAG_MAIN, "Failed to acquire event from pool. Dropping RS485 packet.");
                continue; // Bỏ qua gói tin này và thử lại trong vòng lặp tiếp theo
            }
            
            if (rx_buffer[4] == 0x0B) { // Kiểm tra Frame Length, Expected frame length for this command [cite: 17]
                if (memcmp(rx_buffer, START_CODE, 4) == 0) {    // Kiểm tra Start Code
                    uint8_t frame_length = rx_buffer[4];
                    uint8_t board_id = rx_buffer[5];
                    uint8_t command_word = rx_buffer[6];
                    bool door_change_status = rx_buffer[8];
                    // Các byte dữ liệu sẽ bắt đầu từ rx_packet_buffer[7]
                    
                    // Kiểm tra checksum
                    uint8_t calculated_checksum = lock_controller_calculate_checksum(rx_buffer, rx_buffer[4] - 1);
                    if (calculated_checksum == rx_buffer[rx_buffer[4] - 1]) {
                        // Điền các trường chung của event_ptr
                        event_ptr->data.controller_response.frame_length = rx_buffer[4];
                        event_ptr->data.controller_response.board_id = rx_buffer[5];
                        event_ptr->data.controller_response.command_word = rx_buffer[6];
                        event_ptr->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                        //Phân loại gói tin dựa trên Command Word
                        switch (command_word) { 
                            case CMD_OPEN_SINGLE_LOCK: // Phản hồi cho lệnh mở 1 cửa   ;Khung gửi: 57 4B 4C 59 | 09 | 01 82 04 | 87; Phản hồi: 57 4B 4C 59 | 0B | 01 82 | 00 04 00 | 85
                                //Chuẩn bị dữ liệu:
                                event_ptr->type = EVENT_TYPE_SINGLE_LOCK_RESULT;
                                event_ptr->data.controller_response.command_status = rx_buffer[7];
                                // Print out full response details
                                ESP_LOGI(TAG_MAIN, "Single Lock Command Response:");
                                ESP_LOGI(TAG_MAIN, "Start Code: 0x%02X 0x%02X 0x%02X 0x%02X", rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3]); 
                                ESP_LOGI(TAG_MAIN, "Frame Length: 0x%02X", rx_buffer[4]);
                                ESP_LOGI(TAG_MAIN, "Board ID: 0x%02X", rx_buffer[5]);
                                ESP_LOGI(TAG_MAIN, "Command Word: 0x%02X", rx_buffer[6]); 
                                ESP_LOGI(TAG_MAIN, "Command Status: 0x%02X", rx_buffer[7]);
                                ESP_LOGI(TAG_MAIN, "Lock Channel: 0x%02X", rx_buffer[8]);
                                ESP_LOGI(TAG_MAIN, "Lock Status: 0x%02X", rx_buffer[9]);
                                ESP_LOGI(TAG_MAIN, "Checksum: 0x%02X", rx_buffer[10]);
                            break;

                            case CMD_UPLOAD_DOOR_STATUS_CHANGE: //F85 Toggle input - door close/ open   57 4B 4C 59 | 0A | 01 85 04 | XX | CS   XX là 00 thì là Cửa đang đóng mở ra
                                //Chuẩn bị dữ liệu:
                                switch (door_change_status) {
                                    case 0:
                                        event_ptr->type = EVENT_TYPE_DOOR_STATUS_CHANGED_OPEN;
                                        event_ptr->data.controller_response.command_status = rx_buffer[7];
                                    break;

                                    case 1:
                                        event_ptr->type = EVENT_TYPE_DOOR_STATUS_CHANGED_CLOSE;
                                        event_ptr->data.controller_response.command_status = rx_buffer[7];
                                    break;

                                    default:
                                        ESP_LOGE("DOOR_CHANGE_STATUS","Abnormal status");
                                    break;
                                }
                                ESP_LOGI(TAG_MAIN, "Single Lock Command Response:");
                                ESP_LOGI(TAG_MAIN, "Start Code: 0x%02X 0x%02X 0x%02X 0x%02X", rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3]); 
                                ESP_LOGI(TAG_MAIN, "Frame Length: 0x%02X", rx_buffer[4]);
                                ESP_LOGI(TAG_MAIN, "Board ID: 0x%02X", rx_buffer[5]);
                                ESP_LOGI(TAG_MAIN, "Command Word: 0x%02X", rx_buffer[6]); 
                                ESP_LOGI(TAG_MAIN, "Lock Channel: 0x%02X", rx_buffer[7]);
                                ESP_LOGI(TAG_MAIN, "Lock Status: 0x%02X", rx_buffer[8]);
                                ESP_LOGI(TAG_MAIN, "Checksum: 0x%02X", rx_buffer[9]);
                                
                            break;

                            default:
                                
                            break;
                        }
                        if (event_ptr != NULL && master_event_queue != NULL ) {
                        
                            if (xQueueSend(master_event_queue, &event_ptr, pdMS_TO_TICKS(100)) != pdPASS) {
                                ESP_LOGE(TAG_MAIN, "Failed to send lock operation event! Releasing buffer.");
                                event_pool_release_buffer(event_ptr); // Release if send fails
                            } else { ESP_LOGI(TAG_MAIN, "Lock operation event sent successfully");
                            // Buffer is now owned by master_event_queue; state_machine_task will release it
                            }
                        }  else {ESP_LOGE(TAG_MAIN, "event_ptr or master queue NULL! Releasing buffer.");
                                    event_pool_release_buffer(event_ptr);} // Release if send fails
                    } else {ESP_LOGE(TAG_MAIN, "Checksum mismatch in response. Calculated: 0x%02X, Received: 0x%02X.Releasing buffer.", calculated_checksum, rx_buffer[rx_buffer[4] - 1]);
                                    event_pool_release_buffer(event_ptr);} // Release if send fails
                } else { ESP_LOGE(TAG_MAIN, "Invalid Start Code in response.Releasing buffer.");
                                    event_pool_release_buffer(event_ptr);} // Release if send fails}
            } else {ESP_LOGE(TAG_MAIN, "Invalid Frame Length in response: 0x%02X, expected 0x0B.", rx_buffer[4]);
                                    event_pool_release_buffer(event_ptr);} // Release if send fails}
    //} else {ESP_LOGE(TAG_MAIN, "No response or error reading response for open single lock.");
      //                              event_pool_release_buffer(event_ptr); // Release if send fails}
            } else if (read_bytes ==0) {
                ESP_LOGD(TAG_MAIN, "No RS485 packet in this cycle. Yielding CPU.");
                // Không có gói tin nào. Nhường CPU cho các task khác.
                vTaskDelay(pdMS_TO_TICKS(5)); 
            } else { ESP_LOGE(TAG_MAIN, "Error during RS485 read operation. Will retry after a short delay.");
            vTaskDelay(pdMS_TO_TICKS(20)); }// Đợi lâu hơn một chút sau lỗi để tránh lặp lại lỗi quá nhanh
    }
}

*/

    

//==========================================================================
// Task để xử lý yêu cầu provisioning từ người dùng
//==========================================================================

static void provisioning_request_task(void *pvParameter) {
    while(1) {
        // Wait for notification from combo timer callback
        uint32_t notification_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if(notification_value > 0) {
            // Acquire a buffer from the pool
            system_event_t* prov_req_evt_ptr = event_pool_acquire_buffer(pdMS_TO_TICKS(10)); // Use a small timeout
            
            if (prov_req_evt_ptr) {
                // Populate the event data
                prov_req_evt_ptr->type = EVENT_TYPE_PROVISIONING_REQUEST;
                prov_req_evt_ptr->timestamp = (uint32_t)(esp_timer_get_time() / 1000);

                // Send the pointer to the event buffer to master queue
                if (master_event_queue != NULL) {
                    if (xQueueSend(master_event_queue, &prov_req_evt_ptr, pdMS_TO_TICKS(100)) != pdPASS) {
                        ESP_LOGE(TAG_MAIN, "Failed to send provisioning request event! Releasing buffer.");
                        event_pool_release_buffer(prov_req_evt_ptr); // Release if send fails
                    } else {
                        ESP_LOGI(TAG_MAIN, "Provisioning request event sent successfully");
                        // Buffer is now owned by master_event_queue; state_machine_task will release it
                    }
                } else {
                    ESP_LOGE(TAG_MAIN, "Master event queue NULL in prov_request_task. Releasing buffer.");
                    event_pool_release_buffer(prov_req_evt_ptr); // Should not happen if init is correct
                }
            } else {
                ESP_LOGE(TAG_MAIN, "Failed to acquire event buffer for provisioning request!");
            }
        }
    }
}

//==========================================================================
// Timer callback for combo keys hold detection
//==========================================================================

void combo_hold_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG_MAIN, "*+# combo hold detected (5 seconds)!");

    // Create the task if it doesn't exist
    if (prov_request_task_handle == NULL) {
        BaseType_t task_created = xTaskCreate(
            provisioning_request_task,
            "ProvRequestTask",
            16384,
            NULL,
            5,
            &prov_request_task_handle
        );
        if (task_created != pdPASS) {
            ESP_LOGE(TAG_MAIN, "Failed to create provisioning request task!");
            return;
        }
    }

    // Notify the task
    if (prov_request_task_handle != NULL) {
        xTaskNotify(prov_request_task_handle, 1, eSetValueWithOverwrite);
    }
}


/*// Task để monitor RSSI
static void wifi_rssi_monitor_task(void *pvParameters) {
    wifi_ap_record_t ap_info;
    
    while (1) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG_MAIN, "WiFi RSSI: %d dBm", ap_info.rssi);
        } else {
            ESP_LOGW(TAG_MAIN, "Failed to get WiFi RSSI");
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // Delay 2 giây
    }
}
*/



//==========================================================================
// Idle timer callback
//==========================================================================
void idle_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG_MAIN, "Idle timer expired!");

    system_event_t* idle_evt = event_pool_acquire_buffer(0);
    if (idle_evt != NULL) {
        idle_evt->type = EVENT_TYPE_TIMER_IDLE;
        idle_evt->timestamp = (uint32_t)(esp_timer_get_time() / 1000);

        if (master_event_queue != NULL) {
            // Send the pointer to the acquired buffer
            if (xQueueSend(master_event_queue, &idle_evt, 0) != pdPASS) {
                ESP_LOGE(TAG_MAIN, "Failed to send idle timeout event to queue!");
                event_pool_release_buffer(idle_evt); // Release if send fails
            }
            // If xQueueSend is successful, the buffer is now owned by the receiving task.
        } else {
            ESP_LOGE(TAG_MAIN, "Master event queue is NULL in idle_timer_callback!"); // Good to log this
            event_pool_release_buffer(idle_evt); // Release buffer if queue is NULL
        }
    } else {
        ESP_LOGE(TAG_MAIN, "Failed to acquire event buffer for idle timeout!");
    }
}

// Reset idle timer
void reset_idle_timer(void) {
    // Chỉ reset nếu không ở standby và timer đã được tạo
    if (current_state != BOX_STATE_STANDBY && idle_timer != NULL) {
        xTimerStop(idle_timer, 0); // Dừng trước cho chắc
        if (xTimerReset(idle_timer, pdMS_TO_TICKS(100)) != pdPASS) {
            ESP_LOGE(TAG_MAIN, "Failed to reset idle timer");
        } else {
             ESP_LOGD(TAG_MAIN, "Idle timer reset.");
        }
    } else if (idle_timer != NULL) {
        // Nếu đang ở standby, đảm bảo timer đã dừng
         xTimerStop(idle_timer, 0);
         ESP_LOGD(TAG_MAIN,"Idle timer stopped (already in standby or timer null).");
    }
}


//==========================================================================
// Hàm khởi tạo - CẦN TỐI ƯU HÓA
//==========================================================================

static void initialize_app(void) {
    esp_err_t ret;

    // Khởi tạo NVS==================================================================
    ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG_MAIN, "Erasing NVS flash...");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
    ESP_ERROR_CHECK(ret); // Dừng nếu NVS lỗi nghiêm trọng
    ESP_LOGI(TAG_MAIN, "NVS Initialized.");

    // 2. Khởi tạo TCP/IP Adapter và Event Loop (QUAN TRỌNG - PHẢI TRƯỚC WIFI INIT)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG_MAIN, "Netif Initialized.");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG_MAIN, "Default Event Loop Created.");


    // 3. Tạo Default WiFi STA (và AP nếu cần cho provisioning)
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); // Bỏ comment nếu provisioning dùng SoftAP do bạn quản lý
    ESP_LOGI(TAG_MAIN, "Default WiFi STA Netif Created.");


    // 4. Khởi tạo WiFi Stack
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG_MAIN, "WiFi Stack Initialized.");

    // 5. (Tùy chọn) Đặt bộ nhớ lưu trữ cấu hình WiFi (RAM hoặc NVS)
    // Nếu wifi_manager của bạn tự đọc/ghi NVS thì có thể dùng WIFI_STORAGE_RAM ở đây
    // để tránh IDF tự động lưu/đọc.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // 6. Đặt chế độ WiFi ban đầu (ví dụ STA, hoặc APSTA nếu provisioning cần)
    // wifi_manager_connect_sta() sẽ đặt lại nếu cần, nhưng nên có một mode ban đầu.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG_MAIN, "WiFi Mode Set to STA.");



    // Initialize LCD (Hàm này sẽ tạo display_task và display_queue)
        ST7920_DisplayInit();
        vTaskDelay(pdMS_TO_TICKS(200)); // Chờ display task sẵn sàng (có thể cần)
        ESP_LOGI(TAG_MAIN, "Display initialized (Task and Queue created)");
        ST7920_SendToQueue("CMD_SHOW_INITIAL_SCREEN");

    // Khởi tạo cấu hình Keypad GPIO
        keypad_config_t keypad_config = {
        // !!! Đảm bảo các chân GPIO này đúng với kết nối thực tế !!!
            .rows = {GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_6},
            .cols = {GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_15, GPIO_NUM_37},

        };
    // Khởi tạo keypad với config và queue
        ret = keypad_init(&keypad_config, master_event_queue); // code mới (truyền queue)
        if (ret != ESP_OK) { /* Lỗi */ return; }
        ESP_LOGI(TAG_MAIN, "Keypad initialized");

    // (Tùy chọn) Cài đặt debounce/hold nếu muốn khác mặc định của component
        keypad_set_debounce_time(50);
        keypad_set_hold_time(5000);

   // Khởi tạo driver RS485
    ret = rs485_driver_init(UART_PORT, UART_TX_PIN, UART_RX_PIN, 9600); // Baud rate 9600 [cite: 1]
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "Failed to initialize RS485 driver: %s", esp_err_to_name(ret));
        while(1) { vTaskDelay(1); }
    }
    ESP_LOGI(TAG_MAIN, "RS485 driver initialized.");

    // Khởi tạo driver bo điều khiển khóa
    ret = lock_controller_driver_init(UART_PORT, 0x00); // Sử dụng địa chỉ mặc định 0x00 cho bo [cite: 7]
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "Failed to initialize lock controller driver: %s", esp_err_to_name(ret));
        while(1) { vTaskDelay(1); }
    }
    ESP_LOGI(TAG_MAIN, "Lock controller driver initialized.");

    // Khởi tạo driver RFID scanner



    // 9. Khởi tạo WiFi Manager (SAU KHI WIFI STACK ĐÃ INIT)
    ret = wifi_manager_init(master_event_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "WiFi manager init failed: %s", esp_err_to_name(ret));
        // Xử lý lỗi nghiêm trọng nếu cần, ví dụ vào state FAULT
        current_state = BOX_STATE_FAULT;
        ST7920_SendToQueue("CMD_SHOW_FAULT_SCREEN");
        return; // Không tiếp tục nếu wifi_manager lỗi
    }
    ESP_LOGI(TAG_MAIN, "WiFi manager initialized");
    
    /*  // Tạo task monitor RSSI
    xTaskCreate(wifi_rssi_monitor_task, "rssi_monitor", 2048, NULL, 1, NULL);
    //ESP_LOGI(TAG_MAIN, "RSSI monitor task created");*/

    // 10. Bắt đầu WiFi Stack (SAU KHI WIFI INIT VÀ SET MODE)
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG_MAIN, "WiFi Stack Started.");

    // 11. Bắt đầu thử kết nối WiFi tự động (đã có, nhưng đặt sau wifi_start)
    ESP_LOGI(TAG_MAIN, "Initial WiFi connection attempt starting...");
    current_state = BOX_STATE_WIFI_CONNECTING;
    
    ret = wifi_manager_connect_sta();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED ) { // ESP_ERR_WIFI_NOT_STARTED có thể OK nếu provisioning bắt đầu
        ESP_LOGE(TAG_MAIN, "Failed to initiate WiFi connection or provisioning: %s", esp_err_to_name(ret));
        // Gửi event lỗi về state machine
        system_event_t* evt_fail = event_pool_acquire_buffer(0);
        if (evt_fail) {
            evt_fail->type = EVENT_TYPE_WIFI_CONNECT_FAIL;
            evt_fail->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
            if (xQueueSend(master_event_queue, &evt_fail, 0) != pdTRUE) {
                ESP_LOGE(TAG_MAIN, "Failed to send initial WIFI_CONNECT_FAIL event!");
                event_pool_release_buffer(evt_fail);
            }
        } else { ESP_LOGE(TAG_MAIN, "Failed to acquire buffer for WIFI_CONNECT_FAIL event in init_app");}
    } else {
        ESP_LOGI(TAG_MAIN, "WiFi connection attempt initiated OR Provisioning started successfully.");
    }


    //==========================
    //OTA  
    //==========================
    ESP_LOGI(TAG_MAIN, "Initializing GitHub OTA...");
    // --- BƯỚC 1: Khai báo và định nghĩa cấu hình ghota ---
        ghota_config_t ghconfig = {
        // hostname, orgname, reponame sẽ lấy từ Kconfig nếu không được đặt ở đây
        .filenamematch = "boxPlus.bin", // !!! THAY THẾ BẰNG TÊN FILE BIN CHÍNH XÁC CỦA BẠN !!!
        .storagenamematch = "", // Để trống nếu không cập nhật storage qua OTA
        .storagepartitionname = "", // Để trống nếu không cập nhật storage qua OTA
        .updateInterval = 2, // Ví dụ: Kiểm tra mỗi 60 phút (tùy chọn)
        };
    // --- BƯỚC 2: Khởi tạo ghota client và lưu handle ---
        ghota_handle = ghota_init(&ghconfig); // Sử dụng ghconfig ở đây
        if (ghota_handle == NULL) {
            ESP_LOGE(TAG_MAIN, "Failed to initialize GitHub OTA client");
            // Xử lý lỗi nghiêm trọng nếu cần
        } else {
            ESP_LOGI(TAG_MAIN, "GitHub OTA client initialized successfully.");
            // (Tùy chọn) Đặt thông tin xác thực nếu repo là private
            // esp_err_t auth_err = ghota_set_auth(ghota_handle, "YOUR_GITHUB_USERNAME", "YOUR_GITHUB_PAT_TOKEN");
            // if (auth_err != ESP_OK) {
            //     ESP_LOGW(TAG_MAIN, "Failed to set GitHub OTA authentication");
            // }
        }
    // --- BƯỚC 3: Đăng ký event handler (đảm bảo gọi sau khi ghota_init thành công) ---
    // Đoạn code này có thể đã có sẵn, chỉ cần đảm bảo nó nằm sau ghota_init
    // và truyền ghota_handle vào tham số thứ 4

        if (ghota_handle) { // Chỉ đăng ký nếu handle hợp lệ
            ret = esp_event_handler_register(GHOTA_EVENTS, ESP_EVENT_ANY_ID, &ghota_event_handler, ghota_handle); // Truyền handle vào handler
            if (ret != ESP_OK) {
                ESP_LOGE(TAG_MAIN, "Failed to register GHOTA event handler: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG_MAIN, "GHOTA event handler registered.");
            }
        }


    // Khởi tạo RTDB Client
        esp_err_t rtdb_init_ret = rtdb_client_init(master_event_queue);
        if (rtdb_init_ret != ESP_OK) {
            ESP_LOGE(TAG_MAIN, "Failed to initialize RTDB Client: %s", esp_err_to_name(rtdb_init_ret));
            // Xử lý lỗi nghiêm trọng nếu cần
        } else {
            ESP_LOGI(TAG_MAIN, "RTDB Client initialized.");
        }

    // Tạo idle timer
        idle_timer = xTimerCreate(
            "IdleTimer", pdMS_TO_TICKS(IDLE_TIMEOUT_MS), pdFALSE, NULL, idle_timer_callback
        );
        if (idle_timer == NULL) { // Lỗi 
        return; }
        // Không start timer ở đây, reset_idle_timer sẽ start khi cần
        ESP_LOGI(TAG_MAIN, "Idle timer created.");

}   // end initialize_app



//===================================================================================================================
// MAIN FUNCTION - CẦN TỐI ƯU HÓA
//===================================================================================================================

void app_main(void) {

    ESP_LOGI(TAG_MAIN, "Starting Application...");
    event_pool_init(); // Khởi tạo pool sự kiện

    // Tạo Queue sự kiện chính
    master_event_queue = xQueueCreate(EVENT_POOL_SIZE + 5, sizeof(system_event_t*));
    if (master_event_queue == NULL) { ESP_LOGE(TAG_MAIN, "Failed to create master_event_queue. Halting."); while(1); }
    ESP_LOGI(TAG_MAIN, "Master event queue (for event pointers) created.");

    //esp_log_level_set("RTDB_CLIENT", ESP_LOG_VERBOSE);
    //esp_log_level_set("wifi_prov_mgr", ESP_LOG_VERBOSE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANS_TCP", ESP_LOG_VERBOSE); // Log cho tầng transport TCP
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);  // Log cho tầng ESP-TLS (MbedTLS)
    esp_log_level_set("Mbed-TLS", ESP_LOG_VERBOSE); // Log cho MbedTLS

    initialize_app(); // Gọi hàm khởi tạo

    BaseType_t prov_task_created = xTaskCreate(provisioning_request_task, "ProvReqTask", 16384, NULL, 5, &prov_request_task_handle);
    if (prov_task_created != pdPASS) { ESP_LOGE(TAG_MAIN, "Failed to create provisioning request task!"); }

    combo_hold_timer = xTimerCreate("ComboHoldTimer", pdMS_TO_TICKS(COMBO_HOLD_TIMEOUT_MS), pdFALSE, (void *)0, combo_hold_timer_callback);
    if (combo_hold_timer == NULL) { ESP_LOGE(TAG_MAIN, "Failed to create combo hold timer!"); return; }
    ESP_LOGI(TAG_MAIN, "Combo hold timer created.");

    BaseType_t sm_task_created = xTaskCreate(state_machine_task, "StateMachineTask", 16384, NULL, 7, NULL);
     if (sm_task_created != pdPASS) { ESP_LOGE(TAG_MAIN, "Failed to create StateMachineTask!"); return; }
    ESP_LOGI(TAG_MAIN, "State Machine Task created.");

    //Tạo task chuyên nhận các sự kiện RS485   
    
    BaseType_t rs485_task_created = xTaskCreate(rs485_RX_task, "RS485EventHandler", 16384, NULL, 8, &rs485_RX_task_handle);
     if (rs485_task_created != pdPASS) { ESP_LOGE(TAG_MAIN, "Failed to create RS485Task!"); return; }
    ESP_LOGI(TAG_MAIN, "RS485 Task created.");


    ESP_LOGI(TAG_MAIN, "System ready.");

    // Vòng lặp chính của app_main có thể không cần làm gì nhiều
    // Hoặc có thể bị xóa nếu không cần thiết sau khi tạo task
     while (1) {
         vTaskDelay(pdMS_TO_TICKS(10000)); // Chờ 5 giây
     }

    // Hoặc để app_main kết thúc nếu không cần làm gì thêm
    // vTaskDelete(NULL); // Tự xóa task app_main
}   // end app_main

//===================================================================================================================