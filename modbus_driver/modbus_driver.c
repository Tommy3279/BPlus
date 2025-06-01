#include <stdio.h>
#include "modbus_driver.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h" // Có thể vẫn cần nếu muốn cấu hình chân TX/RX
#include "string.h"
#include "lock_controller_driver.h"

static const char *TAG = "RS485_DRIVER";
static uart_port_t s_uart_port_num; //Khai báo biến nội bộ, lưu tạm uart_num từ bên ngoài truyền vào

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

int rs485_driver_read_data(uart_port_t uart_num, uint8_t *data, size_t max_len, TickType_t total_packet_timeout) {
    if (uart_num != s_uart_port_num || data == NULL || max_len == 0) {
        ESP_LOGE(TAG, "Invalid parameters for read_data.");
        return -1;
    }

    uint32_t start_time = xTaskGetTickCount();
    int bytes_read_total = 0;
    uint8_t current_byte;
    int consecutive_no_bytes_read_count = 0;

    // Bước 1: Tìm Start Code (0x57 0x4B 0x4C 0x59)
    // Đọc từng byte cho đến khi tìm thấy Start Code hoặc hết timeout
    while (bytes_read_total < 4) {  //START_CODE_LEN
        if (xTaskGetTickCount() - start_time >= total_packet_timeout) {
            ESP_LOGD(TAG, "Timeout while waiting for Start Code.");
            return 0; // Không nhận được gói tin nào trong timeout
        }
        // Timeout nhỏ cho từng byte, đảm bảo CPU không bị block quá lâu.
        // Đây là điểm cốt lõi giúp task không bị kẹt.
        int bytes_read = uart_read_bytes(uart_num, &current_byte, 1, pdMS_TO_TICKS(5)); 

        if (bytes_read > 0) {
            consecutive_no_bytes_read_count = 0; // Reset bộ đếm nếu có byte được đọc
            if (bytes_read_total == 4) {    //START_CODE_LEN
                memmove(data, data + 1, 3); //Nếu buffer đã đầy mà chưa tìm thấy Start Code, dịch chuyển để có chỗ cho byte mới và bỏ đi byte cũ nhất.
                bytes_read_total--;
            }
            data[bytes_read_total] = current_byte;
            bytes_read_total++;

            // Kiểm tra xem chuỗi hiện tại có khớp với Start Code không
            // Đây là một cách kiểm tra Start Code linh hoạt hơn.
            // Nếu có 1 byte không khớp, ta vẫn giữ lại các byte đã khớp trước đó và dịch chuyển
            // để có thể bắt đầu tìm kiếm lại Start Code từ byte tiếp theo trong stream.
            bool match_found = true;
            if (bytes_read_total == 4) {    //START_CODE_LEN
                if (memcmp(data, START_CODE, 4) != 0) {
                    match_found = false;
                }
            } else {
                // Kiểm tra từng byte con khi đang trong quá trình tìm Start Code
                if (data[bytes_read_total - 1] != START_CODE[bytes_read_total - 1]) {
                     // Nếu byte hiện tại không khớp với vị trí mong đợi của SC
                     // Dịch chuyển buffer để byte hiện tại có thể là byte đầu tiên của SC mới
                     memmove(data, data + (bytes_read_total - 1), 1); // Chỉ giữ lại byte hiện tại
                     bytes_read_total = 1; // Số byte đã đọc là 1
                     if (data[0] != START_CODE[0]) {
                         bytes_read_total = 0; // Nếu byte hiện tại cũng không phải SC[0], reset hoàn toàn
                     }
                }
            }
            if (bytes_read_total == 4 && !match_found) {    //START_CODE_LEN
                // Đã đọc đủ 4 byte nhưng không khớp SC.
                // Dịch chuyển buffer để bỏ byte đầu tiên và tìm kiếm SC mới từ byte thứ 2 trở đi.
                memmove(data, data + 1, 3);
                bytes_read_total--;
            }

        } else {
            // Không có byte nào trong timeout của uart_read_bytes (5ms)
            consecutive_no_bytes_read_count++;
            // Nếu đã không đọc được byte nào trong một thời gian đáng kể, hãy chủ động yield
            if (consecutive_no_bytes_read_count % 5 == 0) { // Ví dụ: sau mỗi 50ms không có byte nào
                vTaskDelay(pdMS_TO_TICKS(1)); // Nhường CPU để các task khác (bao gồm idle task) chạy
            }
        }
    }
    
    // Đã nhận được Start Code. Tiếp tục đọc Frame Length.
    // Frame Length nằm ở data[4]
    int bytes_to_read_frame_len = 1;
    // Tính toán timeout còn lại
    TickType_t remaining_timeout = total_packet_timeout - (xTaskGetTickCount() - start_time);
    if (remaining_timeout <= 0) {
        ESP_LOGD(TAG, "Timeout before reading Frame Length.");
        return 0; // Hoàn toàn hết timeout
    }

    if (bytes_read_total + bytes_to_read_frame_len > max_len) {
        ESP_LOGE(TAG, "Buffer too small for Frame Length after Start Code.");
        return -1;
    }
    
    // Read Frame Length
    int read_result = uart_read_bytes(uart_num, &data[bytes_read_total], bytes_to_read_frame_len, remaining_timeout);
    if (read_result != bytes_to_read_frame_len) {
        ESP_LOGE(TAG, "Timeout or incomplete read for Frame Length. Expected %d, Got %d.", bytes_to_read_frame_len, read_result);
        return -1; // Trả về lỗi
    }
    bytes_read_total += bytes_to_read_frame_len; // Bây giờ đã nhận được 4 (SC) + 1 (FL) = 5 bytes

    uint8_t received_frame_length = data[4]; // Giá trị Frame Length thực tế từ gói tin
    ESP_LOGD(TAG, "Received Frame Length: %d bytes. Expecting total packet len: %d bytes", (int)received_frame_length, (int)received_frame_length);

    // Kiểm tra Frame Length hợp lệ
    // Tối thiểu 9 bytes: 4 (SC) + 1 (FL) + 1 (Board ID) + 1 (Command Word) + 1 (Data (min)) + 1 (Checksum)
    // Và phải nhỏ hơn hoặc bằng kích thước buffer tối đa cho phép
    if (received_frame_length < 9 || received_frame_length > max_len) {
        ESP_LOGE(TAG, "Invalid Frame Length received: %d. Must be >=9 and <=%zu. Resetting.", (int)received_frame_length, max_len);
        return -1; // Trả về lỗi
    }

    // Đọc các byte còn lại của gói tin
    // Số byte còn lại = total_frame_length - bytes_already_read (Start Code + Frame Length)
    int remaining_bytes_to_read = received_frame_length - bytes_read_total;
    
    if (remaining_bytes_to_read > 0) { // Đảm bảo có byte để đọc
        remaining_timeout = total_packet_timeout - (xTaskGetTickCount() - start_time);
        if (remaining_timeout <= 0) {
            ESP_LOGD(TAG, "Timeout before reading remaining bytes.");
            return 0; // Hoàn toàn hết timeout
        }

        if (bytes_read_total + remaining_bytes_to_read > max_len) {
            ESP_LOGE(TAG, "Calculated packet length (%d) exceeds buffer size (%zu). Frame Length: %d.", (int)received_frame_length, max_len, (int)received_frame_length);
            return -1; // Buffer không đủ
        }
        read_result = uart_read_bytes(uart_num, &data[bytes_read_total], remaining_bytes_to_read, remaining_timeout);

        if (read_result != remaining_bytes_to_read) {
            ESP_LOGE(TAG, "Timeout or incomplete read for remaining packet bytes. Expected %d, Got %d.", remaining_bytes_to_read, read_result);
            return -1; // Trả về lỗi
        }
        bytes_read_total += remaining_bytes_to_read; // Tổng số byte đã nhận
    }

    ESP_LOGI(TAG, "Full packet received (%d bytes) from RS485_DRIVER:", bytes_read_total);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, bytes_read_total, ESP_LOG_INFO);

    return bytes_read_total; // Trả về tổng số byte của gói tin đã nhận được
}



/*
int rs485_driver_read_data(uart_port_t uart_num, uint8_t *data, size_t len, TickType_t timeout_ticks) {
    if (uart_num != s_uart_port_num) {
        ESP_LOGE(TAG, "Invalid UART port number for read.");
        return -1;
    }
    // Đọc dữ liệu. Module MAX485 sẽ luôn ở chế độ RX khi không có dữ liệu được gửi.
    int rx_bytes = uart_read_bytes(uart_num, data, len, timeout_ticks);
        if (rx_bytes < 0) {
            ESP_LOGE(TAG, "Error reading data from UART");
        } else if (rx_bytes == 0) {
            ESP_LOGD(TAG, "No data received within timeout period");
        } else {
            ESP_LOGD(TAG, "Read %d bytes", rx_bytes);
        }
    return rx_bytes;    
}*/

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