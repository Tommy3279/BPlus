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

// Include các file header của dự án
#include "box_system.h"   // Chứa định nghĩa system_event_t, box_state_t, KEY_EVENT_...
#include "keypad16.h"     // Component keypad đã sửa đổi theo Phương án 1
#include "compartments.h" // Chứa định nghĩa compartment_t, NUM_COMPARTMENTS, các trạng thái compartment
#include "display.h"    // Chứa định nghĩa cho LCD (ST7920)
#include "wifi_manager.h"   
#include "event_pool_manager.h"
// Khai báo cho event pool
// --- Event Pool Manager ---
#define EVENT_POOL_SIZE 10 // Số lượng event trong pool
static system_event_t event_pool[EVENT_POOL_SIZE];
static QueueHandle_t free_event_slots_queue;
QueueHandle_t master_event_queue; // Queue giờ sẽ chứa system_event_t*



// Forward declaration of the GHOTA event handler
static void ghota_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

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
#define COMBO_HOLD_TIMEOUT_MS 5000 // 5 giây

#define MAX_WIFI_RETRIES 5
#define WIFI_RETRY_DELAY_MS 5000 // Chờ 5 giây trước khi thử lại
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


#define FIREBASE_WEB_API_KEY_MAIN "AIzaSyDTtRXLnsYXLARXk_LBtd47Aegimp3D9XI" // << THAY THẾ KEY CỦA BẠN
#define FIREBASE_DATABASE_URL_MAIN "https://sample2us-8f23b-default-rtdb.firebaseio.com" // << THAY THẾ URL CỦA BẠN
#define TEST_RTDB_PATH "/testNode" // Ví dụ path


static void clear_input_buffers() {
    memset(phone_number, 0, sizeof(phone_number));
    phone_length = 0;
    //memset(selected_compartment, 0, sizeof(selected_compartment));
    //selected_compartment = 0;
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
                        case BOX_STATE_STANDBY:
                            ESP_LOGD(TAG_MAIN, "[State: STANDBY] Processing Keypad Event");

                            // Chỉ xử lý khi phím được NHẤN
                            if (kp_data.type == KEYPAD_PRESSED) { // <-- Kiểm tra type con của keypad
                                current_state = BOX_STATE_SELECT_COMPARTMENT;
                            } else {
                            // Bỏ qua sự kiện RELEASED hoặc HOLD trong state này
                                ESP_LOGD(TAG_MAIN,"Ignoring RELEASED/HOLD event for key '%c' in STANDBY", kp_data.key_char);
                            }
                            break; // BOX_STATE_STANDBY

                        case BOX_STATE_SELECT_COMPARTMENT: //[OK]
                            if (kp_data.type == KEYPAD_PRESSED) {
                                char key = kp_data.key_char;
                                // Kiểm tra xem có phải phím chọn ô tủ (A-D) không
                                bool compartment_found = false;
                                for (int i = 0; i < NUM_COMPARTMENTS; i++) {
                                    if (key == compartments[i].name) {
                                        compartment_found = true;
                                        ESP_LOGI(TAG_MAIN, "Compartment '%c' selected. State: %d", key, compartments[i].state);
                                        selected_compartment = key; // Lưu ô tủ được chọn

                                        // Kiểm tra trạng thái ô tủ và hiển thị [OK]
                                        if (compartments[i].state == FULL_STATE) {
                                            ESP_LOGI(TAG_MAIN, "Action: Display 'Tu %c da day'", key);
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

                        case BOX_STATE_INPUT_PHONE_NUM:
                             ESP_LOGD(TAG_MAIN, "[State: INPUT_PHONE_NUM] Processing Keypad Event");
                             if (kp_data.type == KEYPAD_PRESSED) {
                                char key = kp_data.key_char;
                                bool phone_updated = false;

                                if (key >= '0' && key <= '9' && phone_length < (PHONE_MAX_LENGTH - 1)) {
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
                                        ESP_LOGI(TAG_MAIN, "Action: Validate phone '%s'. Transitioning to VALIDATING_PHONE_NUM", phone_number);
                                        // Gửi lệnh yêu cầu xác thực SĐT
                                        char buffer[64];
                                        snprintf(buffer, sizeof(buffer), "REQ_VALIDATE_PHONE:%s", phone_number);
                                        // TODO: Gửi request này đi đâu đó (MQTT/HTTP task?)
                                        //Thiết lập Firebase path để xác thực SĐT
                                        char rtdb_verifyPN_path[64];
                                        //thêm "u" vào trước phone_number để phù hợp với định dạng Firebase RTDB
                                        snprintf(rtdb_verifyPN_path, sizeof(rtdb_verifyPN_path), "/uLock/u%s", phone_number);

                                        //DEBUG: In this example, we will just log the path
                                        ESP_LOGI(TAG_MAIN, "Firebase RTDB path for phone verification: %s", rtdb_verifyPN_path);

                                        //Gửi SDT đến Firebase RTDB để xác thực



                                        //ESP_LOGI(TAG_MAIN, "Requesting data from RTDB path: %s", TEST_RTDB_PATH);
                                        esp_err_t rtdb_get_err = rtdb_client_get_data(FIREBASE_DATABASE_URL_MAIN, rtdb_verifyPN_path);
                                        if (rtdb_get_err != ESP_OK) {
                                            ESP_LOGE(TAG_MAIN, "Failed to get data from RTDB path '%s': %s", rtdb_verifyPN_path, esp_err_to_name(rtdb_get_err));
                                            ST7920_SendToQueue("CMD_TEMP_MSG:Khong the xac thuc SDT:Vui long thu lai");
                                            current_state = BOX_STATE_STANDBY; // Quay về trạng thái chờ
                                            break;
                                        } else {
                                            ESP_LOGI(TAG_MAIN, "RTDB request sent successfully for phone verification.");

                                        }




                                        current_state = BOX_STATE_VALIDATING_PHONE_NUM;
                                        ST7920_SendToQueue("CMD_NOTICE2:Dang xac thuc SDT..."); // Ví dụ
                                        
                                    } else {
                                         ESP_LOGW(TAG_MAIN, "Action: Send display cmd 'SDT khong hop le'");
                                         ST7920_SendToQueue("CMD_TEMP_MSG:SDT khong hop le::"); // Ví dụ lệnh lỗi tạm thời
                                    }
                                    //xóa SĐT đã nhập
                                    clear_input_buffers(); // Xóa SĐT đã nhập
                                    //Xóa hiển thị tạm thời
                                    
                                
                                }
                                // Nếu SĐT thay đổi, gửi lệnh cập nhật display
                                if (phone_updated) {
                                    char buffer[64];
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
                            //if (kp_data.type == KEYPAD_PRESSED && kp_data.key_char == '*') {
                            //    ESP_LOGI(TAG_MAIN, "Action: Cancel validation. Transitioning back to STANDBY");
                            //    current_state = BOX_STATE_STANDBY;  // Gửi lệnh cập nhật display ở cuối
                            //}
                           break; // BOX_STATE_VALIDATING_PHONE_NUM

                        case BOX_STATE_WIFI_PROVISIONING:
                           if (kp_data.type == KEYPAD_PRESSED && kp_data.key_char == '*') {
                               ESP_LOGI(TAG_MAIN, "Action: Cancel provisioning by keypress. Transitioning back to STANDBY");
                               stop_wifi_provisioning();
                               current_state = BOX_STATE_STANDBY;  // Gửi lệnh cập nhật display ở cuối
                           }
                           // Bỏ qua các keypad event khác
                           break; // BOX_STATE_WIFI_PROVISIONING

                        default:
                           ESP_LOGW(TAG_MAIN, "Keypad event ignored in state %d", current_state);
                           break;
                    } // end switch(current_state) for KEYPAD event
                    break; // EVENT_TYPE_KEYPAD
                } // end case EVENT_TYPE_KEYPAD
            
            //=====================================================================
            //CASE xử lý WF       

            case EVENT_TYPE_WIFI_CONNECTED:
                ESP_LOGI(TAG_MAIN, "WiFi Connected event received by SM.");
                    wifi_retry_count = 0;
                    char buffer[64];
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
                                // Đọc lại các giá trị từ NVS để kiểm tra - DEUG ONLY
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

            case EVENT_TYPE_WIFI_CONNECT_FAIL:
                
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

                break;
            
            case EVENT_TYPE_WIFI_DISCONNECTED:
                    ESP_LOGW(TAG_MAIN, "WiFi disconnected. Attempting to reconnect or go to provisioning.");
                    current_state = BOX_STATE_WIFI_CONNECTING; // Thử kết nối lại
                    wifi_manager_connect_sta(); // Gọi hàm kết nối lại từ wifi_manager
                    break;

            case EVENT_TYPE_PROVISIONING_REQUEST:
                    ESP_LOGI(TAG_MAIN, "Provisioning Request Event Received");
                    // Luôn cho phép vào provisioning từ bất kỳ state nào trừ chính nó
                    if (current_state != BOX_STATE_WIFI_PROVISIONING) {
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
                                // reset_idle_timer(); // State machine sẽ tự vẽ lại màn hình chờ ở cuối
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
                //tạo biến string lấy tên state
                    char state_name[32];
                    sprintf(state_name, "State: %d", current_state);
                 ST7920_SendToQueue(state_name);
                 if(current_state != BOX_STATE_STANDBY && current_state != BOX_STATE_WIFI_PROVISIONING && current_state != BOX_STATE_INITIALIZING) {
                    reset_idle_timer();
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
        // >>> THÊM CODE NÀY ĐỂ GỬI THÔNG BÁO LÊN LCD <<<
            char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
            snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:OTA Thanh Cong:Dang khoi dong lai...");
            ST7920_SendToQueue(msg_buf);
        // <<< KẾT THÚC CODE THÊM >>>
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
// Combo key hold Task handle & timer callback
//==========================================================================


// Task function to handle provisioning request
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

// Modified timer callback
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

    // REMOVE THESE UNUSED LINES:
    // system_event_t idle_event;
    // idle_event.type = EVENT_TYPE_TIMER_IDLE;
    // idle_event.timestamp = (uint32_t)(esp_timer_get_time() / 1000);

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


/**
 * @brief Gửi lệnh cập nhật màn hình tương ứng với state
 * (Hàm helper)


 void update_display_state(box_state_t state_to_display) {
    ESP_LOGD(TAG_MAIN, "Sending display update command for state %d", state_to_display);
    switch(state_to_display) {
        case BOX_STATE_INITIALIZING:
        ST7920_SendToQueue("CMD_SHOW_INITIAL_SCREEN");
            break;
        case BOX_STATE_WIFI_CONNECTING:
            ST7920_SendToQueue("CMD_SHOW_WIFI_CONNECTED");
            break;
        case BOX_STATE_STANDBY:
           ST7920_SendToQueue("CMD_SHOW_STANDBY");
           break;
        case BOX_STATE_SELECT_COMPARTMENT:
           ST7920_SendToQueue("CMD_SHOW_COMPARTMENTS");
           break;
    
       case BOX_STATE_WIFI_PROVISIONING:
            ST7920_SendToQueue("CMD_SHOW_PROV_SCREEN"); // Gửi lệnh khi vào state
            break;
        case BOX_STATE_FAULT:
            ST7920_SendToQueue("CMD_SHOW_FAULT_SCREEN");
            break;
        default:
            ESP_LOGW(TAG_MAIN,"No specific display update defined for state %d", state_to_display);
            ST7920_SendToQueue("CMD_SHOW_STANDBY");
            break;
    }
}
 */

//==========================================================================
// Hàm khởi tạo
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

}



//===================================================================================================================
// MAIN FUNCTION
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