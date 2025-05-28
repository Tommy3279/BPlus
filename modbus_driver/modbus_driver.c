#include <stdio.h>
#include "modbus_driver.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h" // Có thể vẫn cần nếu muốn cấu hình chân TX/RX
#include "string.h"

static const char *TAG = "RS485_DRIVER";
static uart_port_t s_uart_port_num;

esp_err_t rs485_driver_init(uart_port_t uart_num, int tx_pin, int rx_pin, int baud_rate) {
    s_uart_port_num = uart_num;

    /* Cấu hình UART */
    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(s_uart_port_num, &uart_config));

    /* Cài đặt chân UART. Chân RTS (rts_pin) sẽ bị bỏ qua vì không dùng để điều khiển DE/RE */
    ESP_ERROR_CHECK(uart_set_pin(s_uart_port_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Nếu rts_pin được truyền vào nhưng không dùng, có thể cảnh báo hoặc bỏ qua

    /* Cài đặt driver UART, buffer kích thước 256 byte cho cả TX và RX */
    // Kích thước buffer có thể điều chỉnh tùy theo nhu cầu
    esp_err_t err = uart_driver_install(s_uart_port_num, 256, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Đối với module MAX485 chỉ có RXD/TXD và không có chân DE/RE,
     * chúng ta không cần cấu hình UART cho chế độ RS485 (UART_MODE_RS485_HALF_DUPLEX)
     * hoặc điều khiển chân RTS. Module được giả định sẽ tự động chuyển hướng.
     */
    ESP_LOGI(TAG, "RS485 driver (no DE/RE control) initialized on UART%d, TX:%d, RX:%d, Baud:%d",
             s_uart_port_num, tx_pin, rx_pin, baud_rate);

    return ESP_OK;
}


int rs485_driver_send_data(uart_port_t uart_num, const uint8_t *data, size_t len) {
    if (uart_num != s_uart_port_num) {
        ESP_LOGE(TAG, "Invalid UART port number for send.");
        return -1;
    }
    // Gửi dữ liệu. Module MAX485 sẽ tự động chuyển sang chế độ TX khi có dữ liệu trên chân TX của ESP32.
    int tx_bytes = uart_write_bytes(uart_num, (const char *)data, len);
    if (tx_bytes < 0) {
        ESP_LOGE(TAG, "Error sending data via UART.");
    } else if (tx_bytes != len) {
        ESP_LOGW(TAG, "Sent %d bytes, but expected %d bytes.", tx_bytes, len);
    } else {
        ESP_LOGD(TAG, "Sent %d bytes.", tx_bytes);
    }
    // Chờ cho tất cả các byte được gửi ra khỏi bộ đệm UART
    uart_wait_tx_done(uart_num, 100 / portTICK_PERIOD_MS); // Chờ tối đa 100ms
    return tx_bytes;
}

int rs485_driver_read_data(uart_port_t uart_num, uint8_t *data, size_t len, TickType_t timeout_ticks) {
    if (uart_num != s_uart_port_num) {
        ESP_LOGE(TAG, "Invalid UART port number for read.");
        return -1;
    }
    // Đọc dữ liệu. Module MAX485 sẽ luôn ở chế độ RX khi không có dữ liệu được gửi.
    int rx_bytes = uart_read_bytes(uart_num, data, len, timeout_ticks);
    if (rx_bytes < 0) {
        ESP_LOGE(TAG, "Error reading data via UART.");
    } else if (rx_bytes > 0) {
        ESP_LOGD(TAG, "Received %d bytes.", rx_bytes);
    }
    return rx_bytes;
}

esp_err_t rs485_driver_deinit(uart_port_t uart_num) {
    if (uart_num != s_uart_port_num) {
        ESP_LOGE(TAG, "Invalid UART port number for deinit.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = uart_driver_delete(uart_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete UART driver: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "RS485 driver on UART%d de-initialized.", uart_num);
    }
    return err;
}