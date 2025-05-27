#include <stdio.h>

#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "lock_controller_driver.h"
#include "modbus_driver.h"

static const char *TAG = "LOCK_DRV";
static uart_port_t s_uart_num;

// Start Code mặc định [cite: 3]
const uint8_t START_CODE[] = {0x57, 0x4B, 0x4C, 0x59};

esp_err_t lock_controller_driver_init(uart_port_t uart_num, uint8_t board_address) {
    s_uart_num = uart_num;
    // Ở đây không cần lưu board_address nếu mỗi lệnh đều có board_address riêng.
    // Chỉ cần đảm bảo rs485_driver đã được init.
    return ESP_OK;
}

uint8_t lock_controller_calculate_checksum(const uint8_t *data, size_t len) {
    uint8_t checksum = 0x00;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

esp_err_t lock_controller_open_single_lock(uint8_t board_address, uint8_t channel_number, lock_controller_response_t *response) {
    // Tham khảo demo: 57 4B 4C 59 09 00 82 01 83 (Open 0# board, 1# door) [cite: 18]
    // Frame structure: Start Code (4) + Frame Length (1) + Board Address (1) + Cmd Word (1) + Data Field (1) + Checksum (1)
    // Frame Length = 4 + 1 + 1 + 1 + 1 + 1 = 9 (0x09) [cite: 18]
    // Data Field for this command: Channel Number (1 byte) [cite: 15]

    uint8_t tx_buffer[9];
    memcpy(tx_buffer, START_CODE, 4); // Start Code [cite: 15]
    tx_buffer[4] = 0x09;               // Frame Length [cite: 15]
    tx_buffer[5] = board_address;      // Board Address [cite: 15]
    tx_buffer[6] = CMD_OPEN_SINGLE_LOCK; // Command Word [cite: 15]
    tx_buffer[7] = channel_number;     // Data Field: Channel Number [cite: 15]
    tx_buffer[8] = lock_controller_calculate_checksum(tx_buffer, 8); // Checksum [cite: 15]

    ESP_LOGI(TAG, "Sending Open Single Lock command: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
             tx_buffer[0], tx_buffer[1], tx_buffer[2], tx_buffer[3], tx_buffer[4],
             tx_buffer[5], tx_buffer[6], tx_buffer[7], tx_buffer[8]);

    int sent_bytes = rs485_driver_send_data(s_uart_num, tx_buffer, sizeof(tx_buffer));
    if (sent_bytes != sizeof(tx_buffer)) {
        ESP_LOGE(TAG, "Failed to send all bytes for open single lock command.");
        return ESP_FAIL;
    }

    // Chờ và đọc phản hồi. Phản hồi cho Open Single Lock: 57 4B 4C 59 0B 00 82 00 01 00 81 [cite: 18]
    // Frame length: 0x0B (11 bytes) [cite: 17]
    // Data Field: Status (1) + Channel Number (1) + Lock Status (1) [cite: 17]
    uint8_t rx_buffer[11]; // Max response size [cite: 17]
    int read_bytes = rs485_driver_read_data(s_uart_num, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(100)); // Timeout 100ms

    if (read_bytes <= 0) {
        ESP_LOGE(TAG, "No response or error reading response for open single lock.");
        return ESP_ERR_TIMEOUT;
    }

    // Kiểm tra Start Code
    if (memcmp(rx_buffer, START_CODE, 4) != 0) {
        ESP_LOGE(TAG, "Invalid Start Code in response.");
        return ESP_ERR_INVALID_STATE;
    }

    // Kiểm tra Frame Length
    if (rx_buffer[4] != 0x0B) { // Expected frame length for this command [cite: 17]
        ESP_LOGE(TAG, "Invalid Frame Length in response: 0x%02X, expected 0x0B.", rx_buffer[4]);
        return ESP_ERR_INVALID_ARG;
    }

    // Kiểm tra Checksum
    uint8_t calculated_checksum = lock_controller_calculate_checksum(rx_buffer, rx_buffer[4] - 1);
    if (calculated_checksum != rx_buffer[rx_buffer[4] - 1]) {
        ESP_LOGE(TAG, "Checksum mismatch in response. Calculated: 0x%02X, Received: 0x%02X", calculated_checksum, rx_buffer[rx_buffer[4] - 1]);
        return ESP_ERR_INVALID_CRC;
    }

    // Phân tích dữ liệu phản hồi
    if (response != NULL) {
        response->board_address = rx_buffer[5];
        response->command_word = rx_buffer[6];
        response->status = rx_buffer[7]; // Status byte [cite: 17]
        response->data.single_door_status.channel_number = rx_buffer[8]; // Channel number [cite: 17]
        response->data.single_door_status.door_state = rx_buffer[9]; // Lock status [cite: 17]
    }

    ESP_LOGI(TAG, "Received response for Open Single Lock: Board:0x%02X, Cmd:0x%02X, Status:0x%02X, Channel:0x%02X, DoorState:0x%02X",
             response->board_address, response->command_word, response->status,
             response->data.single_door_status.channel_number, response->data.single_door_status.door_state);

    return ESP_OK;
}