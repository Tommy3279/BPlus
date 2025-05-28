#ifndef MODBUS_DRIVER_H
#define MODBUS_DRIVER_H

#include "esp_err.h"
#include "driver/uart.h"

typedef enum {
    RS485_DIR_RX = 0,
    RS485_DIR_TX
} rs485_direction_t;

/**
 * @brief Cấu hình và khởi tạo driver RS485.
 * @param uart_num Số UART để sử dụng (e.g., UART_NUM_2).
 * @param tx_pin Chân GPIO cho TX.
 * @param rx_pin Chân GPIO cho RX.
 * @param rts_pin Chân GPIO cho điều khiển hướng (DE/RE) của MAX485.
 * @param baud_rate Tốc độ baud.
 * @return esp_err_t ESP_OK nếu thành công, ngược lại là lỗi.
 */
esp_err_t rs485_driver_init(uart_port_t uart_num, int tx_pin, int rx_pin, int baud_rate);

/**
 * @brief Gửi dữ liệu qua RS485.
 * @param uart_num Số UART.
 * @param data Con trỏ tới dữ liệu cần gửi.
 * @param len Độ dài dữ liệu.
 * @return int Số byte đã gửi, hoặc lỗi nếu < 0.
 */
int rs485_driver_send_data(uart_port_t uart_num, const uint8_t *data, size_t len);

/**
 * @brief Đọc dữ liệu từ RS485.
 * @param uart_num Số UART.
 * @param data Con trỏ tới buffer để lưu dữ liệu nhận được.
 * @param len Kích thước tối đa của buffer.
 * @param timeout_ms Thời gian chờ nhận dữ liệu (ms).
 * @return int Số byte đã đọc, hoặc lỗi nếu < 0.
 */
int rs485_driver_read_data(uart_port_t uart_num, uint8_t *data, size_t len, TickType_t timeout_ticks);

/**
 * @brief Giải phóng tài nguyên driver RS485.
 * @param uart_num Số UART.
 * @return esp_err_t ESP_OK nếu thành công, ngược lại là lỗi.
 */
esp_err_t rs485_driver_deinit(uart_port_t uart_num);

#endif // MODBUS_DRIVER_H