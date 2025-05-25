#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h" // Thêm FreeRTOS header nếu cần dùng kiểu dữ liệu của nó (như QueueHandle_t)
#include "freertos/queue.h"   // Thêm queue.h cho QueueHandle_t
#include "box_system.h"       // Phụ thuộc vào file này để có định nghĩa system_event_t

/**
 * @brief Khởi tạo WiFi Manager.
 *
 * Hàm này chỉ lưu lại handle của queue sự kiện chính.
 * Việc khởi tạo NVS, TCP/IP, Event Loop, WiFi stack cần được thực hiện ở nơi khác (vd: app_main)
 * trước khi gọi start_wifi_provisioning.
 *
 * @param event_queue Handle của queue mà WiFi Manager sẽ gửi sự kiện vào.
 * @return esp_err_t Luôn trả về ESP_OK trong phiên bản này.
 */
esp_err_t wifi_manager_init(QueueHandle_t event_queue);


esp_err_t wifi_manager_deinit(void);



/**
 * @brief Yêu cầu WiFi Manager thử kết nối vào Access Point đã lưu.
 *
 * Hàm này sẽ đặt WiFi ở chế độ STA và gọi esp_wifi_connect().
 * Kết quả thành công (có IP) hoặc thất bại sẽ được báo về qua master_queue
 * thông qua các sự kiện EVENT_TYPE_WIFI_CONNECTED hoặc EVENT_TYPE_WIFI_CONNECT_FAIL.
 *
 * Yêu cầu: wifi_manager_init() đã được gọi.
 *
 * @return esp_err_t ESP_OK nếu yêu cầu kết nối được bắt đầu, mã lỗi nếu thất bại.
 */
esp_err_t wifi_manager_connect_sta(void);

/**
 * @brief Bắt đầu quá trình WiFi provisioning sử dụng SoftAP.
 *
 * Yêu cầu: NVS, TCP/IP, Event Loop, WiFi stack đã được khởi tạo.
 * Sẽ đăng ký handler cho sự kiện provisioning và bắt đầu SoftAP.
 *
 * @return esp_err_t ESP_OK nếu thành công, mã lỗi nếu thất bại.
 */
esp_err_t start_wifi_provisioning(void);

/**
 * @brief Dừng quá trình WiFi provisioning đang chạy.
 *
 * Sẽ dừng SoftAP, hủy đăng ký event handler và giải phóng tài nguyên provisioning manager.
 *
 * @return esp_err_t ESP_OK nếu thành công, mã lỗi nếu thất bại.
 */
esp_err_t stop_wifi_provisioning(void);


#endif // WIFI_MANAGER_H