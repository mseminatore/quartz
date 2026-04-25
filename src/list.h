//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Internal priority-sorted singly-linked list.
// Used by the ready list and blocked list in the scheduler.
//---------------------------------------------------------------------------
#ifndef RTOS_LIST_H
#define RTOS_LIST_H

#include "rtos_task.h"

// Insert tcb into the list headed by *head, sorted ascending by key.
// For the ready list the key is priority (lower = higher priority).
// For IPC wait lists the key is priority (same ordering).
void list_insert_sorted(rtos_tcb_t **head, rtos_tcb_t *tcb, uint32_t key);

// Insert into a blocked list (G_BLOCKED) using signed comparison so that
// wakeup_tick values that span the uint32_t rollover are ordered correctly.
void list_insert_sorted_signed(rtos_tcb_t **head, rtos_tcb_t *tcb, uint32_t key);

// Insert at the tail (for FIFO within same priority).
void list_insert_tail(rtos_tcb_t **head, rtos_tcb_t *tcb);

// Remove a specific tcb from the list. Returns 1 if found and removed.
int list_remove(rtos_tcb_t **head, rtos_tcb_t *tcb);

// Remove and return the head (highest priority / soonest deadline).
rtos_tcb_t *list_pop_head(rtos_tcb_t **head);

// Return the head without removing it.
rtos_tcb_t *list_peek_head(rtos_tcb_t *head);

#endif // RTOS_LIST_H
