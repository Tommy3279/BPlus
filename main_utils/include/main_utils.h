#ifndef MAIN_UTILS_H
#define MAIN_UTILS_H

#include "box_system.h"
#include <esp_ghota.h>   //OTA

//Master event queue
extern QueueHandle_t master_event_queue; // Queue giờ sẽ chứa system_event_t*

// Khai báo cho event pool
#define EVENT_POOL_SIZE 10 // Số lượng event trong pool
extern system_event_t event_pool[EVENT_POOL_SIZE];
extern QueueHandle_t free_event_slots_queue;


extern box_state_t current_state; // Trạng thái hiện tại của state machine

// Định nghĩa GPIO cho module MAX485
#define UART_PORT UART_NUM_2
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_16
//#define UART_RTS_PIN GPIO_NUM_18 // Hoặc chân GPIO điều khiển DE/RE của MAX485

// OTA
extern ghota_client_handle_t *ghota_handle;


// --- Timer cho trạng thái không hoạt động (Idle) ---
//#define IDLE_TIMEOUT_MS 30000 // Ví dụ: 30 giây không hoạt động thì về Standby
extern TimerHandle_t idle_timer;



//Khai báo hàm
void time_sync_sntp(void);
void initialize_app(void);
void ghota_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void idle_timer_callback(TimerHandle_t xTimer);
void reset_idle_timer(void);




#endif  //MAIN_UTILS_H