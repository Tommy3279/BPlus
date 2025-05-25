#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdbool.h>
#include "esp_rom_sys.h"
#include "ST7920_SERIAL.h"
#include <sys/types.h>
#include "bitmap.h"
#include "font.h"
#include "f6x8m.h"
#include "f10x16f.h"
#include "compartments.h"
#include "freertos/queue.h" 
#include "compartments.h" 

#define DISPLAY_MESSAGE_MAX_LEN 64
#define DISPLAY_QUEUE_SIZE 10

// Phone status enum
typedef enum {
    PHONE_STATUS_INPUTTING,
    PHONE_STATUS_OK,
    PHONE_STATUS_TOO_LONG,
    PHONE_STATUS_DONE
} phone_status_t;

// Function declarations
// void display_init(void); // Có thể không cần hàm này nữa
// void display_update_phone(const char *phone_number, phone_status_t status); // Hàm này có thể không cần nữa
void ST7920_DisplayInit(void); // Hàm khởi tạo chính (tạo task, queue)
// esp_err_t ST7920_DisplayTaskInit(void); // Có thể gộp vào ST7920_DisplayInit
esp_err_t ST7920_SendToQueue(const char *message); // Hàm gửi lệnh chính

void display_compartments(compartment_t *compartments, int num_compartments);

#endif /* DISPLAY_H_ */