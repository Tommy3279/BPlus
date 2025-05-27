// main/event_pool_manager.h (hoặc include/event_pool_manager.h)
#ifndef EVENT_POOL_MANAGER_H
#define EVENT_POOL_MANAGER_H

#include "freertos/FreeRTOS.h" // Cho TickType_t
#include "box_system.h"       // Cho system_event_t

/**
 * @brief Initializes the event pool and the queue for free event slots.
 * Should be called once at application startup before the master event queue is created.
 */
void event_pool_init(void);

/**
 * @brief Acquires an event buffer from the pool.
 *
 * This function attempts to receive a pointer to a free system_event_t buffer
 * from the free_event_slots_queue.
 *
 * @param timeout The maximum amount of time (in ticks) the task should block
 * waiting for a buffer to become available.
 * @return system_event_t* Pointer to an acquired event buffer, or NULL if
 * no buffer is available within the timeout period or
 * if the pool is not initialized.
 * The acquired buffer will be memset to 0.
 */
system_event_t* event_pool_acquire_buffer(TickType_t timeout);

/**
 * @brief Releases an event buffer back to the pool.
 *
 * This function sends the pointer of the processed event buffer back to the
 * free_event_slots_queue, making it available for reuse.
 *
 * @param event_ptr Pointer to the system_event_t buffer to be released.
 * Should not be NULL.
 */
void event_pool_release_buffer(system_event_t* event_ptr);

#endif // EVENT_POOL_MANAGER_H
