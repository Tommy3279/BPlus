#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // Thêm queue.h
#include "main_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h" // Cho ESP-IDF 5.0 trở lên
#include "esp_wifi.h"    // Cần cho ví dụ trigger sau khi có IP
#include "esp_event.h"   // Cần cho ví dụ trigger sau khi có IP

#include "nvs_flash.h"
#include "rtdb_client.h"
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

#define TAG_UTILS "MAIN_UTILS"


// Định nghĩa (cấp phát bộ nhớ) biến toàn cục master_event_queue
QueueHandle_t master_event_queue; // Queue giờ sẽ chứa system_event_t*

// Định nghĩa event pool
//#define EVENT_POOL_SIZE 10 // Số lượng event trong pool
system_event_t event_pool[EVENT_POOL_SIZE];
QueueHandle_t free_event_slots_queue;

// OTA
ghota_client_handle_t *ghota_handle = NULL;

// Trạng thái hiện tại của state machine
box_state_t current_state = BOX_STATE_STANDBY; 

// --- Timer cho trạng thái không hoạt động (Idle) ---
#define IDLE_TIMEOUT_MS 30000 // Ví dụ: 30 giây không hoạt động thì về Standby
TimerHandle_t idle_timer = NULL;

//==========================================================================
// Hàm cho event pool
//==========================================================================
// Hàm khởi tạo event pool (gọi một lần trong app_main trước khi tạo master_event_queue)
void event_pool_init(void) {
    free_event_slots_queue = xQueueCreate(EVENT_POOL_SIZE, sizeof(system_event_t*));
    if (free_event_slots_queue == NULL) {
        ESP_LOGE(TAG_UTILS, "Failed to create free_event_slots_queue!");
        // Xử lý lỗi nghiêm trọng
        return;
    }
    for (int i = 0; i < EVENT_POOL_SIZE; i++) {
        // Gửi con trỏ của từng event trong pool vào queue các slot rảnh
        system_event_t* event_ptr = &event_pool[i];
        if (xQueueSend(free_event_slots_queue, &event_ptr, 0) != pdPASS) {
            ESP_LOGE(TAG_UTILS, "Failed to populate free_event_slots_queue at index %d", i);
        }
    }
    ESP_LOGI(TAG_UTILS, "Event pool initialized with %d slots.", EVENT_POOL_SIZE);
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
            ESP_LOGW(TAG_UTILS, "Failed to acquire event buffer from pool (timeout or queue empty).");
        }
    } else {
        ESP_LOGE(TAG_UTILS, "Free event slots queue not initialized!");
    }
    return NULL; // Trả về NULL nếu không mượn được
}

// Hàm "trả" một buffer event về pool
void event_pool_release_buffer(system_event_t* event_ptr) {
    if (free_event_slots_queue != NULL && event_ptr != NULL) {
        // Không cần kiểm tra có thuộc pool không nếu logic acquire/release đúng
        if (xQueueSend(free_event_slots_queue, &event_ptr, 0) != pdPASS) {
            ESP_LOGE(TAG_UTILS, "Failed to release event buffer back to pool!");
        }
    } else { ESP_LOGW(TAG_UTILS, "Attempted to release NULL event_ptr or free_event_slots_queue not init."); }
}
// --- End Event Pool Manager ---
//==========================================================================


//==========================================================================
// Hàm đồng bộ thời gian với SNTP
//==========================================================================
void time_sync_sntp(void) {
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
// Idle timer callback
//==========================================================================
void idle_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG_UTILS, "Idle timer expired!");

    system_event_t* idle_evt = event_pool_acquire_buffer(0);
    if (idle_evt != NULL) {
        idle_evt->type = EVENT_TYPE_TIMER_IDLE;
        idle_evt->timestamp = (uint32_t)(esp_timer_get_time() / 1000);

        if (master_event_queue != NULL) {
            // Send the pointer to the acquired buffer
            if (xQueueSend(master_event_queue, &idle_evt, 0) != pdPASS) {
                ESP_LOGE(TAG_UTILS, "Failed to send idle timeout event to queue!");
                event_pool_release_buffer(idle_evt); // Release if send fails
            }
            // If xQueueSend is successful, the buffer is now owned by the receiving task.
        } else {
            ESP_LOGE(TAG_UTILS, "Master event queue is NULL in idle_timer_callback!"); // Good to log this
            event_pool_release_buffer(idle_evt); // Release buffer if queue is NULL
        }
    } else {
        ESP_LOGE(TAG_UTILS, "Failed to acquire event buffer for idle timeout!");
    }
}

// Reset idle timer
void reset_idle_timer(void) {
    // Chỉ reset nếu không ở standby và timer đã được tạo
    if (current_state != BOX_STATE_STANDBY && idle_timer != NULL) {
        xTimerStop(idle_timer, 0); // Dừng trước cho chắc
        if (xTimerReset(idle_timer, pdMS_TO_TICKS(100)) != pdPASS) {
            ESP_LOGE(TAG_UTILS, "Failed to reset idle timer");
        } else {
             ESP_LOGD(TAG_UTILS, "Idle timer reset.");
        }
    } else if (idle_timer != NULL) {
        // Nếu đang ở standby, đảm bảo timer đã dừng
         xTimerStop(idle_timer, 0);
         ESP_LOGD(TAG_UTILS,"Idle timer stopped (already in standby or timer null).");
    }
}


//==========================================================================
// Hàm khởi tạo - CẦN TỐI ƯU HÓA
//==========================================================================

void initialize_app(void) {
    esp_err_t ret;

    // 1. Khởi tạo NVS==================================================================
    ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG_UTILS, "Erasing NVS flash...");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
    ESP_ERROR_CHECK(ret); // Dừng nếu NVS lỗi nghiêm trọng
    ESP_LOGI(TAG_UTILS, "NVS Initialized.");

    // 2. Khởi tạo TCP/IP Adapter và Event Loop (QUAN TRỌNG - PHẢI TRƯỚC WIFI INIT)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG_UTILS, "Netif Initialized.");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG_UTILS, "Default Event Loop Created.");

    // 3. Tạo Default WiFi STA (và AP nếu cần cho provisioning)
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); // Bỏ comment nếu provisioning dùng SoftAP do bạn quản lý
    ESP_LOGI(TAG_UTILS, "Default WiFi STA Netif Created.");

    // 4. Khởi tạo WiFi Stack
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG_UTILS, "WiFi Stack Initialized.");

    // 5. (Tùy chọn) Đặt bộ nhớ lưu trữ cấu hình WiFi (RAM hoặc NVS)
    // Nếu wifi_manager của bạn tự đọc/ghi NVS thì có thể dùng WIFI_STORAGE_RAM ở đây
    // để tránh IDF tự động lưu/đọc.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // 6. Đặt chế độ WiFi ban đầu (ví dụ STA, hoặc APSTA nếu provisioning cần)
    // wifi_manager_connect_sta() sẽ đặt lại nếu cần, nhưng nên có một mode ban đầu.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG_UTILS, "WiFi Mode Set to STA.");

    // 7. Initialize LCD (Hàm này sẽ tạo display_task và display_queue)
        ST7920_DisplayInit();
        vTaskDelay(pdMS_TO_TICKS(200)); // Chờ display task sẵn sàng (có thể cần)
        ESP_LOGI(TAG_UTILS, "Display initialized (Task and Queue created)");
        ST7920_SendToQueue("CMD_SHOW_INITIAL_SCREEN");

    // 8.1 Khởi tạo cấu hình Keypad GPIO
        keypad_config_t keypad_config = {
        // !!! Đảm bảo các chân GPIO này đúng với kết nối thực tế !!!
            .rows = {GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_6},
            .cols = {GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_15, GPIO_NUM_37},

        };
    // 8.2 Khởi tạo keypad với config và queue
        ret = keypad_init(&keypad_config, master_event_queue); // code mới (truyền queue)
        if (ret != ESP_OK) { /* Lỗi */ return; }
        ESP_LOGI(TAG_UTILS, "Keypad initialized");

    // 8.3 Cài đặt debounce/hold nếu muốn khác mặc định của component
        keypad_set_debounce_time(50);
        keypad_set_hold_time(5000);
    
    // Khởi tạo driver RS485
    ret = rs485_driver_init(UART_PORT, UART_TX_PIN, UART_RX_PIN, 9600); // Baud rate 9600 [cite: 1]
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UTILS, "Failed to initialize RS485 driver: %s", esp_err_to_name(ret));
        while(1) { vTaskDelay(1); }
    }
    ESP_LOGI(TAG_UTILS, "RS485 driver initialized.");

    // Khởi tạo driver bo điều khiển khóa
    ret = lock_controller_driver_init(UART_PORT, 0x00); // Sử dụng địa chỉ mặc định 0x00 cho bo [cite: 7]
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UTILS, "Failed to initialize lock controller driver: %s", esp_err_to_name(ret));
        while(1) { vTaskDelay(1); }
    }
    ESP_LOGI(TAG_UTILS, "Lock controller driver initialized.");

    // Khởi tạo driver RFID scanner


    // 9. Khởi tạo WiFi Manager (SAU KHI WIFI STACK ĐÃ INIT)
    ret = wifi_manager_init(master_event_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UTILS, "WiFi manager init failed: %s", esp_err_to_name(ret));
        // Xử lý lỗi nghiêm trọng nếu cần, ví dụ vào state FAULT
        current_state = BOX_STATE_FAULT;
        ST7920_SendToQueue("CMD_SHOW_FAULT_SCREEN");
        return; // Không tiếp tục nếu wifi_manager lỗi
    }
    ESP_LOGI(TAG_UTILS, "WiFi manager initialized");
    
    /*  // Tạo task monitor RSSI
    xTaskCreate(wifi_rssi_monitor_task, "rssi_monitor", 2048, NULL, 1, NULL);
    //ESP_LOGI(TAG_UTILS, "RSSI monitor task created");*/

    // 10. Bắt đầu WiFi Stack (SAU KHI WIFI INIT VÀ SET MODE)
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG_UTILS, "WiFi Stack Started.");

    // 11. Bắt đầu thử kết nối WiFi tự động (đã có, nhưng đặt sau wifi_start)
    ESP_LOGI(TAG_UTILS, "Initial WiFi connection attempt starting...");
    current_state = BOX_STATE_WIFI_CONNECTING;
    
    ret = wifi_manager_connect_sta();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED ) { // ESP_ERR_WIFI_NOT_STARTED có thể OK nếu provisioning bắt đầu
        ESP_LOGE(TAG_UTILS, "Failed to initiate WiFi connection or provisioning: %s", esp_err_to_name(ret));
        // Gửi event lỗi về state machine
        system_event_t* evt_fail = event_pool_acquire_buffer(0);
        if (evt_fail) {
            evt_fail->type = EVENT_TYPE_WIFI_CONNECT_FAIL;
            evt_fail->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
            if (xQueueSend(master_event_queue, &evt_fail, 0) != pdTRUE) {
                ESP_LOGE(TAG_UTILS, "Failed to send initial WIFI_CONNECT_FAIL event!");
                event_pool_release_buffer(evt_fail);
            }
        } else { ESP_LOGE(TAG_UTILS, "Failed to acquire buffer for WIFI_CONNECT_FAIL event in init_app");}
    } else {
        ESP_LOGI(TAG_UTILS, "WiFi connection attempt initiated OR Provisioning started successfully.");
    }


    //==========================
    //12. OTA  
    //==========================
    ESP_LOGI(TAG_UTILS, "Initializing GitHub OTA...");
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
            ESP_LOGE(TAG_UTILS, "Failed to initialize GitHub OTA client");
            // Xử lý lỗi nghiêm trọng nếu cần
        } else {
            ESP_LOGI(TAG_UTILS, "GitHub OTA client initialized successfully.");
            // (Tùy chọn) Đặt thông tin xác thực nếu repo là private
            // esp_err_t auth_err = ghota_set_auth(ghota_handle, "YOUR_GITHUB_USERNAME", "YOUR_GITHUB_PAT_TOKEN");
            // if (auth_err != ESP_OK) {
            //     ESP_LOGW(TAG_UTILS, "Failed to set GitHub OTA authentication");
            // }
        }
    // --- BƯỚC 3: Đăng ký event handler (đảm bảo gọi sau khi ghota_init thành công) ---
    // Đoạn code này có thể đã có sẵn, chỉ cần đảm bảo nó nằm sau ghota_init
    // và truyền ghota_handle vào tham số thứ 4

        if (ghota_handle) { // Chỉ đăng ký nếu handle hợp lệ
            ret = esp_event_handler_register(GHOTA_EVENTS, ESP_EVENT_ANY_ID, &ghota_event_handler, ghota_handle); // Truyền handle vào handler
            if (ret != ESP_OK) {
                ESP_LOGE(TAG_UTILS, "Failed to register GHOTA event handler: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG_UTILS, "GHOTA event handler registered.");
            }
        }
}
        
//==========================================================================
// Hàm xử lý sự kiện từ esp_ghota
//==========================================================================

void ghota_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
ESP_LOGI(TAG_UTILS, "GHOTA Event received: base=%s, event_id=%" PRIi32, event_base, event_id);

// Thêm switch case để xử lý các loại sự kiện GHOTA
switch(event_id) {
    case GHOTA_EVENT_START_CHECK:
        ESP_LOGI(TAG_UTILS, "GHOTA: Checking for new release");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_UPDATE_AVAILABLE:
        ESP_LOGI(TAG_UTILS, "GHOTA: New Version Available");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_NOUPDATE_AVAILABLE:
        ESP_LOGI(TAG_UTILS, "GHOTA: No Update Available");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_START_UPDATE:
        ESP_LOGI(TAG_UTILS, "GHOTA: Starting Firmware Update");
        // TODO: Cập nhật display nếu cần
        break;
    case GHOTA_EVENT_FINISH_UPDATE:
        ESP_LOGI(TAG_UTILS, "GHOTA: Firmware Update Successful");
            char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
            snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:OTA Thanh Cong:Dang khoi dong lai...");
            ST7920_SendToQueue(msg_buf);
        break;

    case GHOTA_EVENT_UPDATE_FAILED:
        ESP_LOGE(TAG_UTILS, "GHOTA: Firmware Update Failed");
        // TODO: Cập nhật display thông báo lỗi
            snprintf(msg_buf, sizeof(msg_buf), "CMD_TEMP_MSG:OTA That Bai:Kiem tra ket noi");
            ST7920_SendToQueue(msg_buf);
        break;
    case GHOTA_EVENT_START_STORAGE_UPDATE:
         ESP_LOGI(TAG_UTILS, "GHOTA: Starting Storage Update");
         break;
    case GHOTA_EVENT_FINISH_STORAGE_UPDATE:
         ESP_LOGI(TAG_UTILS, "GHOTA: Storage Update Successful");
         break;
    case GHOTA_EVENT_STORAGE_UPDATE_FAILED:
         ESP_LOGE(TAG_UTILS, "GHOTA: Storage Update Failed");
         break;
    case GHOTA_EVENT_FIRMWARE_UPDATE_PROGRESS: {
        int progress = *(int*)event_data;
        ESP_LOGV(TAG_UTILS, "GHOTA: Firmware Update Progress: %d%%", progress);
        // TODO: Cập nhật display thanh tiến trình
        // Có thể gửi lệnh dạng "CMD_PROGRESS:firmware:%d"
        char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
        snprintf(msg_buf, sizeof(msg_buf), "CMD_PROGRESS:firmware:%d", progress);
        ST7920_SendToQueue(msg_buf);

        break;
    }
    case GHOTA_EVENT_STORAGE_UPDATE_PROGRESS: {
         int progress = *(int*)event_data;
         ESP_LOGV(TAG_UTILS, "GHOTA: Storage Update Progress: %d%%", progress);
         // TODO: Cập nhật display thanh tiến trình
         // Có thể gửi lệnh dạng "CMD_PROGRESS:storage:%d"
         char msg_buf[DISPLAY_MESSAGE_MAX_LEN];
         snprintf(msg_buf, sizeof(msg_buf), "CMD_PROGRESS:storage:%d", progress);
         ST7920_SendToQueue(msg_buf);
         
         break;
    }
    case GHOTA_EVENT_PENDING_REBOOT:
         ESP_LOGI(TAG_UTILS, "GHOTA: Reboot Pending");
         // TODO: Cập nhật display thông báo khởi động lại

            ST7920_SendToQueue("CMD_TEMP_MSG:Khoi dong lai...");

         break;
    default:
        ESP_LOGW(TAG_UTILS, "Unknown GHOTA Event ID: %" PRIi32, event_id);
        break;
}

    // 14. Khởi tạo RTDB Client
        esp_err_t rtdb_init_ret = rtdb_client_init(master_event_queue);
        if (rtdb_init_ret != ESP_OK) {
            ESP_LOGE(TAG_UTILS, "Failed to initialize RTDB Client: %s", esp_err_to_name(rtdb_init_ret));
            // Xử lý lỗi nghiêm trọng nếu cần
        } else {
            ESP_LOGI(TAG_UTILS, "RTDB Client initialized.");
        }


    // Tạo idle timer
        idle_timer = xTimerCreate(
            "IdleTimer", pdMS_TO_TICKS(IDLE_TIMEOUT_MS), pdFALSE, NULL, idle_timer_callback
        );
        if (idle_timer == NULL) { // Lỗi 
        return; }
        // Không start timer ở đây, reset_idle_timer sẽ start khi cần
        ESP_LOGI(TAG_UTILS, "Idle timer created.");

}   // end initialize_app
