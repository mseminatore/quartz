//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Master include — pull in the full RTOS API via a single header.
//---------------------------------------------------------------------------
#ifndef RTOS_H
#define RTOS_H

#include "rtos_config.h"
#include "rtos_task.h"
#include "rtos_sem.h"
#include "rtos_mutex.h"
#include "rtos_queue.h"
#include "rtos_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the scheduler. This function never returns.
void rtos_start(void);

#ifdef __cplusplus
}
#endif

#endif // RTOS_H
