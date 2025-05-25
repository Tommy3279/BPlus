#include <stdio.h>
#include <string.h> // Cho strncpy, strtok_r
#include <stdlib.h> // Cho strtok_r
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // Đảm bảo include queue.h
#include "esp_log.h"
#include "esp_err.h"
#include "ST7920_SERIAL.h" // Include driver cấp thấp
#include <stdbool.h>
#include "compartments.h" // Chứa định nghĩa compartment_t, NUM_COMPARTMENTS
#include "box_system.h"   // Chứa định nghĩa state, event
#include "font.h"         // Include các file font
#include "f6x8m.h"
#include "f10x16f.h"

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 1000
#endif


#define PHONE_MAX_LENGTH 11

#define TAG_DISPLAY "ST7920_DISPLAY"


// Global variables for display task
static QueueHandle_t display_queue = NULL;
static TaskHandle_t display_task_handle = NULL;

static struct {
    char title[32];
    char phone[32];
    phone_status_t status;
    char notice1[32];
    char notice2[32];
} display_data_phone_input = {
    .title = "Nhap SDT nguoi nhan", // Giá trị mặc định
    .phone = "",
    .notice1 = "SDT hop le co 10 so", // Giá trị mặc định
    .notice2 = ""
};



#define ROW_START_Y 14     // Starting Y position for first row
#define ROW_HEIGHT 14     // Height for each row
#define LEFT_COL_X 2      // Left column starting X
#define RIGHT_COL_X 65    // Right column starting X (halfway point)
#define TU_OFFSET 8       // Offset for "Tu" text
#define NAME_OFFSET 23    // Offset for compartment name
#define STATUS_OFFSET 33  // Offset for status text


// --- Khai báo Biến Extern ---
// Khai báo rằng biến `compartments` được định nghĩa ở file khác (ví dụ: main.c)
// Cần include header định nghĩa NUM_COMPARTMENTS và compartment_t (ví dụ: compartments.h)
extern compartment_t compartments[NUM_COMPARTMENTS];


//==========================================================================
// Khai báo các hàm vẽ phụ (static)
//==========================================================================

// --- Khai báo Hàm Nội bộ (Prototypes) ---
static void display_task(void *pvParameters);
static void draw_initial_screen(void);
static void draw_WF_connected_screen(void);
static void draw_standby_screen(void);
static void draw_select_compartment_screen(void);
static void draw_phone_input_screen(void);
static void draw_provisioning_screen(void);
static void draw_fault_screen (void);
static void draw_temp_message(const char* line1, const char* line2);
// Prototype cho các hàm được định nghĩa ở dưới nhưng gọi ở trên
void display_compartments(compartment_t *comps, int num_compartments);
static void draw_compartment_row(int row, compartment_t *comp);  


//==========================================================================
// Các hàm vẽ màn hình cụ thể
//==========================================================================


// Vẽ màn hình khởi động
static void draw_initial_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing initializing Screen");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1); // Đảm bảo graphic mode
    disp1color_printf(1, 22, FONTID_10X16F, "Initializing...");
    // Hoặc hiển thị logo/thông tin khác
    disp1color_UpdateFromBuff();
}


// Vẽ màn hình báo kết nối WF
static void draw_WF_connected_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing WF connected Screen");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1); // Đảm bảo graphic mode
    disp1color_printf(1, 22, FONTID_10X16F, "WF CONNECTED");
    // Hoặc hiển thị logo/thông tin khác
    disp1color_UpdateFromBuff();
}


// Vẽ màn hình chờ Standby
static void draw_standby_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing Standby Screen");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1); // Đảm bảo graphic mode
    disp1color_printf(2, 2, FONTID_10X16F, "2.Press key...");
    // Hoặc hiển thị logo/thông tin khác
    disp1color_UpdateFromBuff();
}

// Vẽ màn hình chọn ô tủ
static void draw_select_compartment_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing Select Compartment Screen");
    // Gọi hàm đã có, đảm bảo biến `compartments` có thể truy cập (nhờ extern)
    display_compartments(compartments, NUM_COMPARTMENTS);
}


// Draw a single compartment row
static void draw_compartment_row(int row, compartment_t *comp) {
    int base_x = (row < 4) ? LEFT_COL_X : RIGHT_COL_X;
    int adjusted_row = (row < 4) ? row : row - 4;
    int y = ROW_START_Y + (adjusted_row * ROW_HEIGHT);
    
    //ESP_LOGI(TAG_DISPLAY, "Drawing compartment %c at x=%d, y=%d", comp->name, base_x, y);

    // Draw rectangle with state
    switch (comp->state) {
        case OK_STATE:
            DrawRectangle(base_x, y, 6, 6);  // Outline only
            break;
            
        case FULL_STATE:
            DrawFilledRectangle(base_x, y, 6, 6);  // Filled
            break;
            
        case FAULT_STATE:
            DrawRectangle(base_x, y, 6, 6);  // Outline
            DrawLine(base_x, y, base_x + 6, y + 6);      // Diagonal 1 
            DrawLine(base_x, y + 6, base_x + 6, y);      // Diagonal 2
            break;
    }
    
    // Draw "Tu" text
    disp1color_printf(base_x + TU_OFFSET, y, FONTID_6X8M, "Tu");
    
    // Draw compartment name
    char name_str[2] = {comp->name, '\0'};
    disp1color_printf(base_x + NAME_OFFSET, y, FONTID_6X8M, name_str);
    
    // Draw status text
    const char *status_str;
    switch (comp->state) {
        case OK_STATE:
            status_str = "OK";
            break;
        case FULL_STATE:
            status_str = "Day";
            break;
        case FAULT_STATE:
            status_str = "Loi";
            break;
        default:
            status_str = "???";
    }
    disp1color_printf(base_x + STATUS_OFFSET, y, FONTID_6X8M, status_str);
}


// Hàm display_compartments (Giữ nguyên logic vẽ)
// Lưu ý: Hàm này cần truy cập biến `compartments`
// --> Cách tốt nhất: Khai báo extern trong compartments.h, định nghĩa ở compartments.c/main.c
//     và cả display.c và main.c cùng include compartments.h

void display_compartments(compartment_t *comps, int num_compartments) {
    ESP_LOGI(TAG_DISPLAY, "Drawing compartments screen via display_compartments function");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1);
    disp1color_printf(LEFT_COL_X, 2, FONTID_6X8M, "Hay chon mot tu:");
    int max_rows = 8;
    int rows_to_draw = (num_compartments < max_rows) ? num_compartments : max_rows;
    for (int i = 0; i < rows_to_draw; i++) {
        // Quan trọng: Truyền con trỏ đúng vào hàm vẽ hàng
        draw_compartment_row(i, &comps[i]);
    }
    disp1color_UpdateFromBuff();
}

/*
// Display all compartments on the LCD
void display_compartments(compartment_t *compartments, int num_compartments) {
    ESP_LOGI(TAG_DISPLAY, "Displaying compartments on LCD");
    
    // Clear the display buffer
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1);
    
    // Draw header text
    disp1color_printf(LEFT_COL_X, 2, FONTID_6X8M, "Hay chon mot tu:");
    
    // Draw each compartment row
    int max_rows = 8;
    int rows_to_draw = (num_compartments < max_rows) ? num_compartments : max_rows;
    
    for (int i = 0; i < rows_to_draw; i++) {
        draw_compartment_row(i, &compartments[i]);
    }

    disp1color_UpdateFromBuff();
}

*/

// Vẽ màn hình nhập SĐT
static void draw_phone_input_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing Phone Input Screen");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1);

    // Vẽ title (line 1)
    disp1color_printf(0, 0, FONTID_6X8M, display_data_phone_input.title);
    // Vẽ phone number (line 2)
    disp1color_printf(0, 16, FONTID_10X16F, display_data_phone_input.phone);
    // Vẽ notice1 (line 3)
    disp1color_printf(0, 40, FONTID_6X8M, display_data_phone_input.notice1);
    // Vẽ notice2 (line 4)
    disp1color_printf(0, 52, FONTID_6X8M, display_data_phone_input.notice2);

    // Update display
    disp1color_UpdateFromBuff();
}

// Vẽ màn hình WiFi Provisioning
static void draw_provisioning_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing Provisioning Screen");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1);
    disp1color_printf(0, 0, FONTID_6X8M, "WiFi Setup Mode");
    disp1color_printf(0, 10, FONTID_6X8M, "Connect to:");
    // TODO: Lấy tên SoftAP thực tế nếu có thể
    disp1color_printf(0, 20, FONTID_6X8M, "BOX-PLUS-XXXXX");
    disp1color_printf(0, 30, FONTID_6X8M, "Via ESP SoftAP Prov");
    disp1color_printf(0, 48, FONTID_6X8M, "Press* to cancel");
    disp1color_UpdateFromBuff();
}

// Vẽ màn hình FAULT
static void draw_fault_screen(void) {
    ESP_LOGI(TAG_DISPLAY, "Drawing FAULT Screen");
    ST7920_Clear();
    disp1color_clearBuff();
    ST7920_GraphicMode(1);
    disp1color_printf(0, 0, FONTID_6X8M, "FALURE");
    disp1color_printf(0, 10, FONTID_6X8M, "Contact admin ASAP");

    disp1color_UpdateFromBuff();
}
// Vẽ một tin nhắn tạm thời (2 dòng)
static void draw_temp_message(const char* line1, const char* line2) {
     ESP_LOGI(TAG_DISPLAY, "Drawing Temp Message: '%s' / '%s'", line1 ? line1 : "NULL", line2 ? line2 : "NULL");
     ST7920_Clear();
     disp1color_clearBuff();
     ST7920_GraphicMode(1);
     if (line1) disp1color_printf(2, 2, FONTID_6X8M, line1);
     if (line2) disp1color_printf(2, 20, FONTID_6X8M, line2); // Điều chỉnh Y nếu cần
     disp1color_UpdateFromBuff();
     // Lưu ý: Tin nhắn này sẽ bị ghi đè bởi lệnh vẽ màn hình tiếp theo.
     // main.c nên có delay sau khi gửi lệnh này nếu muốn nó hiển thị đủ lâu.
}

// Task để xử lý dữ liệu từ Queue và hiển thị lên màn hình LCD:
static void display_task(void *pvParameters) {
    char message[DISPLAY_MESSAGE_MAX_LEN];
    char *cmd_ptr; // Con trỏ dùng cho strtok_r
    
    while (1) {
        // Chờ nhận dữ liệu từ Queue
        if (xQueueReceive(display_queue, message, portMAX_DELAY)) {
            //debug
            ESP_LOGI(TAG_DISPLAY, "Received message: %s", message);

            // Phân tích và xử lý lệnh
            if (strcmp(message, "State: 0") == 0) { //SHOW initializing screen
                draw_initial_screen();
            }else if (strcmp(message, "State: 2") == 0) {   //SHOW standby screen
                draw_standby_screen();
            }else if (strcmp(message, "CMD_SHOW_WIFI_CONNECTED") == 0) {    //SHOW WiFi connected screen
                draw_WF_connected_screen();
            } else if (strcmp(message, "State: 3") == 0) {  //SHOW select compartment screen
                draw_select_compartment_screen(); // Gọi hàm vẽ màn hình chọn tủ
            } else if (strcmp(message, "CMD_SHOW_PROV_SCREEN") == 0) {
                draw_provisioning_screen();
            } else if (strcmp(message, "CMD_SHOW_FAULT_SCREEN") == 0) {
                draw_fault_screen();
            } else if (strncmp(message, "CMD_TITLE:", 10) == 0) {
                ESP_LOGI(TAG_DISPLAY, "CMD_TITLE: %s", message + 10); // Debug
                char *payload = message + 10;
                // Tách chuỗi payload thành 3 phần bằng dấu ":"
                char *line1 = strtok_r(payload, ":", &cmd_ptr); // Dùng strtok_r để tách chuỗi
                char *line3 = strtok_r(NULL, ":", &cmd_ptr);
                // Kiểm tra và sao chép an toàn vào các trường cấu trúc
                    if (line1) {
                        // strncpy copy tối đa sizeof(title)-1 ký tự và đảm bảo kết thúc bằng null
                        strncpy(display_data_phone_input.title, line1, sizeof(display_data_phone_input.title) - 1);
                        display_data_phone_input.title[sizeof(display_data_phone_input.title) - 1] = '\0'; // Đảm bảo null-termination
                    } else {
                        display_data_phone_input.title[0] = '\0'; // Nếu không có dữ liệu, đảm bảo chuỗi rỗng
                    }

                    if (line3) {
                        strncpy(display_data_phone_input.notice1, line3, sizeof(display_data_phone_input.notice1) - 1);
                        display_data_phone_input.notice1[sizeof(display_data_phone_input.notice1) - 1] = '\0';
                    } else {
                        display_data_phone_input.notice1[0] = '\0';
                    }

                draw_phone_input_screen(); // Vẽ lại màn hình nhập SĐT
            } else if (strncmp(message, "CMD_PHONE:", 10) == 0) {
                strncpy(display_data_phone_input.phone, message + 10, sizeof(display_data_phone_input.phone) - 1);
                display_data_phone_input.phone[sizeof(display_data_phone_input.phone) - 1] = '\0';
                // Cập nhật notice dựa trên độ dài phone
                 size_t len = strlen(display_data_phone_input.phone);
                 if (len >= 10) {
                    if(len == 10) { strncpy(display_data_phone_input.notice1, "SDT OK - bam # de mo", sizeof(display_data_phone_input.notice1)-1); display_data_phone_input.notice2[0] = '\0'; }
                    else { strncpy(display_data_phone_input.notice1, "SDT qua nhieu so", sizeof(display_data_phone_input.notice1)-1); strncpy(display_data_phone_input.notice2, "Bam * de xoa bot", sizeof(display_data_phone_input.notice2)-1); }
                 } else { strncpy(display_data_phone_input.notice1, "SDT hop le co 10 so", sizeof(display_data_phone_input.notice1)-1); display_data_phone_input.notice2[0] = '\0'; }
                 display_data_phone_input.notice1[sizeof(display_data_phone_input.notice1)-1] = '\0';
                 display_data_phone_input.notice2[sizeof(display_data_phone_input.notice2)-1] = '\0';
                draw_phone_input_screen(); // Vẽ lại màn hình nhập SĐT
            } else if (strncmp(message, "CMD_NOTICE1:", 12) == 0) {
                 strncpy(display_data_phone_input.notice1, message + 12, sizeof(display_data_phone_input.notice1) - 1);
                 display_data_phone_input.notice1[sizeof(display_data_phone_input.notice1) - 1] = '\0';
                 draw_phone_input_screen();
            } else if (strncmp(message, "CMD_NOTICE2:", 12) == 0) {
                 strncpy(display_data_phone_input.notice2, message + 12, sizeof(display_data_phone_input.notice2) - 1);
                 display_data_phone_input.notice2[sizeof(display_data_phone_input.notice2) - 1] = '\0';
                 draw_phone_input_screen();
            } else if (strncmp(message, "CMD_TEMP_MSG:", 13) == 0) {
                char *payload = message + 13;
                char *line1 = strtok_r(payload, ":", &cmd_ptr);
                char *line2 = strtok_r(NULL, ":", &cmd_ptr);
                draw_temp_message(line1, line2);
            } else if (strncmp(message, "CMD WF:", 7) == 0) {  //"CMD WF: WiFi Retry: %d/%d", wifi_retry_count, MAX_WIFI_RETRIES);
                char *payload = message + 7;
                char *line1 = strtok_r(payload, ":", &cmd_ptr);
                char *line2 = strtok_r(NULL, "/", &cmd_ptr);
                draw_temp_message(line1, line2);
            }

            else {
                ESP_LOGW(TAG_DISPLAY, "Unknown display command: %s", message);
            }
        } // end if xQueueReceive
    } // end while(1)
} // end display_task

//==========================================================================
// Hàm Khởi tạo và Gửi Queue 
//==========================================================================

// Khởi tạo Queue và Task Display
void ST7920_DisplayInit(void) {
    pin_init();
    ST7920_Init();

    // Clear the display buffer initially
    disp1color_clearBuff();

    // Tạo Queue
    display_queue = xQueueCreate(DISPLAY_QUEUE_SIZE, DISPLAY_MESSAGE_MAX_LEN * sizeof(char)); // Sửa sizeof item
    if (display_queue == NULL) {
        ESP_LOGE(TAG_DISPLAY, "Failed to create display queue");
        return;
    }

    // Tạo Task xử lý hiển thị
    BaseType_t task_created = xTaskCreate(
        display_task,          // Task function
        "display_task",        // Task name
        16384,                  // Stack size (Có thể cần tăng nếu logic vẽ phức tạp)
        NULL,                  // Parameters
        5,                     // Priority (Giữ nguyên hoặc điều chỉnh nếu cần)
        &display_task_handle   // Task handle
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG_DISPLAY, "Failed to create display task");
        vQueueDelete(display_queue); // Dọn dẹp queue nếu tạo task lỗi
        display_queue = NULL; // Đặt lại handle
        return;
    }

    ESP_LOGI(TAG_DISPLAY, "Display task and queue initialized");
    // Không cần gọi return ở đây
    // Đoạn code ST7920_Clear(), GraphicMode(1) phía dưới là unreachable, đã xóa.
}

//Hàm để gửi dữ liệu vào Queue từ các thành phần khác:
esp_err_t ST7920_SendToQueue(const char *message) {
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display_queue == NULL) {
        ESP_LOGE(TAG_DISPLAY, "Display queue not initialized yet!");
        return ESP_ERR_INVALID_STATE;
    }

    // Không cần tạo msg_buffer và strncpy nếu message đã đảm bảo độ dài
    // Chỉ cần truyền con trỏ message vào queue là đủ vì queue copy dữ liệu
    // Tuy nhiên, cần đảm bảo message không dài hơn kích thước item của queue (DISPLAY_MESSAGE_MAX_LEN)
    if (strlen(message) >= DISPLAY_MESSAGE_MAX_LEN) {
         ESP_LOGE(TAG_DISPLAY, "Message too long for display queue: %s", message);
         return ESP_ERR_INVALID_ARG; // Hoặc cắt bớt nếu muốn
    }

    // Gửi con trỏ tới chuỗi message vào queue với timeout 0
    if (xQueueSend(display_queue, (void *)message, pdMS_TO_TICKS(10)) != pdPASS) { // Thêm timeout nhỏ
        ESP_LOGE(TAG_DISPLAY, "Failed to send message to display queue (maybe full?): %s", message);
        return ESP_FAIL; // ESP_ERR_NO_MEM không phù hợp lắm, ESP_FAIL chung hơn
    }

    return ESP_OK;
}
