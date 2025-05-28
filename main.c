#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // Thêm queue.h
#include "freertos/semphr.h" // Cho semaphore nếu dùng (queue thường đã đủ thread-safe cho mục đích này)

#include <string.h> // Thêm string.h cho memset


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
#include "lock_controller_driver.h" // Thư viện điều khiển khóa (nếu có)
#include "main_utils.h" // Thư viện tiện ích chung



#define TAG_MAIN "MAIN_APP"


// compartment select message timeout
#define TEMP_MESSAGE_TIMEOUT_MS 2000

// --- Biến cục bộ cho logic state machine ---
#define PHONE_MAX_LENGTH 11 // Độ dài tối đa SĐT + 1 (cho null terminator)
static char phone_number[PHONE_MAX_LENGTH] = {0}; // Lưu SĐT đang nhập
static int phone_length = 0; // Độ dài SĐT hiện tại
static char selected_compartment = '\0'; // Ô tủ đang được chọn


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

void state_machine_task(void *pvParameter);


void combo_hold_timer_callback(TimerHandle_t xTimer);
void update_display_state(box_state_t state_to_display);
static void clear_input_buffers(void);

static TaskHandle_t prov_request_task_handle = NULL;// Task handle for provisioning request task
static void provisioning_request_task(void *pvParameter);
static void validate_phone_number_task(void *pvParameter);

#define FIREBASE_WEB_API_KEY_MAIN "AIzaSyDTtRXLnsYXLARXk_LBtd47Aegimp3D9XI" // << THAY THẾ KEY CỦA BẠN
#define FIREBASE_DATABASE_URL_MAIN "https://sample2us-8f23b-default-rtdb.firebaseio.com" // << THAY THẾ URL CỦA BẠN
#define TEST_RTDB_PATH "/testNode" // Ví dụ path


static void clear_input_buffers() {
    memset(phone_number, 0, sizeof(phone_number));
    phone_length = 0;
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
                                        
                                        //Tạo task xác thực SĐT
                                        xTaskCreate(
                                            validate_phone_number_task, // Hàm task xác thực SĐT
                                            "ValidatePhoneTask",         // Tên task
                                            10240,                        // Stack size
                                            NULL,                        // Parameter (không cần)
                                            5,                           // Priority
                                            NULL                         // Task handle (không cần)
                                        );
                                        
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
            
            case EVENT_TYPE_PHONE_VALIDATED:    //Nhận được sự kiện xác thực SĐT thành công
            {
                ESP_LOGI(TAG_MAIN, "Phone validated event received by SM.");
                ESP_LOGI(TAG_MAIN, "OPENING COMPARTMENT: %c", selected_compartment);
                // Gọi hàm mở ô tủ
                //compartment_open(selected_compartment); // Giả định hàm này sẽ mở ô tủ và cập nhật trạng thái
                // Sau đó tác vụ sẽ gửi sự kiện EVENT_TYPE_COMPARTMENT_OPENED

                break;
            }

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

            case EVENT_TYPE_LOCK_COMMAND_TRIGGER:
            {
                
            }
                break;
            case EVENT_TYPE_LOCK_RESPONSE_RECEIVED:
            {
                ESP_LOGI(TAG_MAIN, "Lock response received. Status: %d", received_event_ptr->data.lock_response.status);
            }

            break;
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

                                    //xóa SĐT đã nhập để tránh nhầm lẫn
                                    clear_input_buffers();
                                    char buffer[DISPLAY_MESSAGE_MAX_LEN];
                                    snprintf(buffer, sizeof(buffer), "CMD_PHONE:%s", phone_number);
                                    ST7920_SendToQueue(buffer);
                                    
                                    ESP_LOGI(TAG_MAIN, "Phone number matches: %s", phone_number);
                                    ESP_LOGI(TAG_MAIN, "Action: Open compartment '%c'", selected_compartment);
                                    //TODO: Gửi lệnh mở ô tủ tương ứng
                                    received_event_ptr->data.lock_channel_to_open = selected_compartment - 'A' + 1; // Giả định kênh khóa tương ứng với ô tủ (A=1, B=2, C=3, D=3)
                                    ESP_LOGI(TAG_MAIN, "Lock channel to open: %d", received_event_ptr->data.lock_channel_to_open);

                                    ESP_LOGI(TAG_MAIN, "EVENT_TYPE_LOCK_COMMAND_TRIGGER: Triggered to open lock channel %d", received_event_ptr->data.lock_channel_to_open);
                                    // Gửi lệnh mở khóa
                                    lock_controller_response_t response;
                                    esp_err_t lock_cmd_err = lock_controller_open_single_lock(0x01, received_event_ptr->data.lock_channel_to_open, &response); // Giả định 0x01 là địa chỉ bo mạch controller, và lock_channel_to_open là kênh cần mở
                                    ESP_LOGI(TAG_MAIN, "Lock open command sent. Response: %d", response.status);
                                    if (lock_cmd_err == ESP_OK) {
                                        ESP_LOGI(TAG_MAIN, "Lock command sent successfully. Response status: %d", response.status);
                                        if (response.status == STATUS_SUCCESS) { // 0x00 cho thành công [cite: 10]
                                            ESP_LOGI(TAG_MAIN, "Lock opened successfully. Response: %d", response.status);
                                            system_event_t success_event = {.type = EVENT_TYPE_LOCK_RESPONSE_RECEIVED, .data.lock_response = response};
                                            xQueueSend(master_event_queue, &success_event, portMAX_DELAY);
                                            current_state = BOX_STATE_OPENING_COMPARTMENT; // Giả sử, có thể chuyển thẳng đến STATE_WAITING_FOR_RESPONSE
                                        } else { // 0xFF cho thất bại [cite: 10]
                                            ESP_LOGE(TAG_MAIN, "Lock command failed with status: %d", response.status);
                                            system_event_t fail_event = {.type = EVENT_TYPE_LOCK_COMMAND_FAILED, .data.lock_response = response};
                                            xQueueSend(master_event_queue, &fail_event, portMAX_DELAY);
                                            current_state = BOX_STATE_FAULT;    // Chuyển sang trạng thái lỗi (tạm thời)
                                        }
                                    } else {
                                        ESP_LOGE(TAG_MAIN, "Error sending open lock command: %s", esp_err_to_name(lock_cmd_err));
                                        system_event_t fail_event = {.type = EVENT_TYPE_LOCK_COMMAND_FAILED}; // Có thể thêm chi tiết lỗi
                                        xQueueSend(master_event_queue, &fail_event, portMAX_DELAY);
                                        current_state = BOX_STATE_FAULT;        // Chuyển sang trạng thái lỗi (tạm thời)
                                    }


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
                    char wf_msg_buffer[DISPLAY_MESSAGE_MAX_LEN];
                    snprintf(wf_msg_buffer, sizeof(wf_msg_buffer), "CMD WF: Dang ket noi lai: %d/%d", wifi_retry_count, MAX_WIFI_RETRIES);
                    ST7920_SendToQueue(wf_msg_buffer); // Hiển thị trạng thái kết nối lại
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
// Task để xác thực SĐT từ RTDB
//==========================================================================
static void validate_phone_number_task(void *pvParameter) {
    while(1) {
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
                current_state = BOX_STATE_STANDBY; // Quay về trạng thái chờ
                vTaskDelete(NULL); // Delete this task
                break;
            } else {    // Nếu gửi thành công
                ESP_LOGI(TAG_MAIN, "RTDB request sent successfully for phone verification.");
                if (phone_evt_ptr) {
                    // Populate the event data
                    phone_evt_ptr->type = EVENT_TYPE_PHONE_VALIDATED;
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

    vTaskDelete(NULL); // Delete this task after sending the request
            
    }
} // end


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