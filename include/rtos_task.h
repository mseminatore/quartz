// Copyright 2025. All rights reserved.
// Task management API.
#ifndef RTOS_TASK_H
#define RTOS_TASK_H

#include <stdint.h>
#include <stddef.h>
#include "rtos_config.h"

typedef void *rtos_handle_t;

// Task states
typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SUSPENDED,
    TASK_DELETED,
} rtos_task_state_t;

// Task Control Block — storage provided by the user as a static variable
typedef struct rtos_tcb {
    void               *sp;                      // saved stack pointer (port-specific type)
    void               *stack_base;              // bottom of stack (for overflow detection)
    size_t              stack_words;
    uint8_t             priority;
    rtos_task_state_t   state;
    uint32_t            delay_ticks;             // countdown for vTaskDelay
    char                name[RTOS_TASK_NAME_LEN];
    struct rtos_tcb    *next;                    // intrusive list link
} rtos_tcb_t;

// Create a task. tcb and stack must be static storage provided by the caller.
// stack_words is the number of RTOS_STACK_BYTES_PER_WORD units in the stack buffer.
rtos_handle_t xTaskCreate(rtos_tcb_t *tcb,
                           void       *stack,
                           size_t      stack_words,
                           void      (*func)(void *),
                           void       *arg,
                           const char *name,
                           uint8_t     priority);

// Delay the calling task for the given number of ticks.
void vTaskDelay(uint32_t ticks);

// Voluntarily yield the CPU to the next ready task.
void vTaskYield(void);

// Suspend / resume a task by handle (pass NULL to target the current task).
void vTaskSuspend(rtos_handle_t task);
void vTaskResume(rtos_handle_t task);

// Remove a task from all lists. Pass NULL to delete the current task.
void vTaskDelete(rtos_handle_t task);

// Return the current tick count.
uint32_t xTaskGetTickCount(void);

#endif // RTOS_TASK_H
