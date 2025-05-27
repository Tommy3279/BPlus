#ifndef LOCK_CONTROLLER_DRIVER_H
#define LOCK_CONTROLLER_DRIVER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "modbus_driver.h"
#include "box_system.h"
// Định nghĩa các mã lệnh
#define CMD_OPEN_MULTIPLE_LOCKS_SIMULTANEOUSLY  0x80 // Đồng thời mở nhiều khóa [cite: 4]
#define CMD_CHANNEL_FLASH                       0x81 // Kênh nhấp nháy (LED) [cite: 4]
#define CMD_OPEN_SINGLE_LOCK                    0x82 // Mở một khóa [cite: 4]
#define CMD_QUERY_SINGLE_DOOR_STATUS            0x83 // Truy vấn trạng thái một cửa [cite: 4]
#define CMD_QUERY_ALL_DOOR_STATUS               0x84 // Truy vấn trạng thái tất cả các cửa [cite: 4]
#define CMD_UPLOAD_DOOR_STATUS_CHANGE           0x85 // Tải lên chủ động khi trạng thái cửa thay đổi [cite: 4]
#define CMD_OPEN_ALL_LOCKS                      0x86 // Mở tất cả các khóa [cite: 4]
#define CMD_OPEN_MULTIPLE_LOCKS_SEQUENTIALLY    0x87 // Mở nhiều khóa (từng cái một) [cite: 4]
#define CMD_CHANNEL_CONTINUOUS_ON               0x88 // Kênh duy trì mở (LED) [cite: 4]
#define CMD_CHANNEL_OFF                         0x89 // Kênh đóng (LED) [cite: 4]

// Định nghĩa các trạng thái
#define STATUS_SUCCESS                          0x00 // Thực hiện thành công [cite: 3]
#define STATUS_FAIL                             0xFF // Thực hiện thất bại [cite: 3]
#define DOOR_STATE_OPEN                         0x00 // Cửa mở [cite: 18]
#define DOOR_STATE_CLOSED                       0x01 // Cửa đóng [cite: 18]




/**
 * @brief Khởi tạo driver cho bo điều khiển khóa.
 * @param uart_num Số UART đã được khởi tạo bởi rs485_driver.
 * @param board_address Địa chỉ mặc định của bo điều khiển (nếu chỉ giao tiếp với 1 bo).
 * @return esp_err_t
 */
esp_err_t lock_controller_driver_init(uart_port_t uart_num, uint8_t board_address);

/**
 * @brief Tính toán Checksum cho khung dữ liệu.
 * @param data Con trỏ tới dữ liệu (bắt đầu từ Start Code).
 * @param len Độ dài dữ liệu (tính đến cuối Data Field).
 * @return uint8_t Giá trị checksum.
 */
uint8_t lock_controller_calculate_checksum(const uint8_t *data, size_t len);

/**
 * @brief Gửi lệnh mở một khóa.
 * @param board_address Địa chỉ của bo điều khiển.
 * @param channel_number Số kênh (cửa) cần mở.
 * @param response Con trỏ tới cấu trúc để lưu phản hồi.
 * @return esp_err_t
 */
esp_err_t lock_controller_open_single_lock(uint8_t board_address, uint8_t channel_number, lock_controller_response_t *response);

/**
 * @brief Gửi lệnh mở nhiều khóa cùng lúc.
 * @param board_address Địa chỉ của bo điều khiển.
 * @param channel_numbers Mảng chứa các số kênh cần mở.
 * @param num_channels Số lượng kênh trong mảng.
 * @param response Con trỏ tới cấu trúc để lưu phản hồi.
 * @return esp_err_t
 */
esp_err_t lock_controller_open_multiple_locks_simultaneously(uint8_t board_address, const uint8_t *channel_numbers, uint8_t num_channels, lock_controller_response_t *response);

/**
 * @brief Gửi lệnh truy vấn trạng thái một cửa.
 * @param board_address Địa chỉ của bo điều khiển.
 * @param channel_number Số kênh (cửa) cần truy vấn.
 * @param response Con trỏ tới cấu trúc để lưu phản hồi.
 * @return esp_err_t
 */
esp_err_t lock_controller_query_single_door_status(uint8_t board_address, uint8_t channel_number, lock_controller_response_t *response);

/**
 * @brief Gửi lệnh truy vấn trạng thái tất cả các cửa.
 * @param board_address Địa chỉ của bo điều khiển.
 * @param response Con trỏ tới cấu trúc để lưu phản hồi.
 * @return esp_err_t
 */
esp_err_t lock_controller_query_all_door_status(uint8_t board_address, lock_controller_response_t *response);

// Thêm các hàm API cho các lệnh khác nếu cần
// ...

#endif // LOCK_CONTROLLER_DRIVER_H