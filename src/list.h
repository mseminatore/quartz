// Copyright 2025. All rights reserved.
// Internal priority-sorted doubly-linked list.
// Used by the ready list and blocked list in the scheduler.
#ifndef RTOS_LIST_H
#define RTOS_LIST_H

#include "rtos_task.h"

// Insert tcb into the list headed by *head, sorted ascending by key.
// For the ready list the key is priority (lower = higher priority).
// For the blocked list the key is delay_ticks.
void list_insert_sorted(rtos_tcb_t **head, rtos_tcb_t *tcb, uint32_t key);

// Insert at the tail (for FIFO within same priority).
void list_insert_tail(rtos_tcb_t **head, rtos_tcb_t *tcb);

// Remove a specific tcb from the list. Returns 1 if found and removed.
int list_remove(rtos_tcb_t **head, rtos_tcb_t *tcb);

// Remove and return the head (highest priority / soonest deadline).
rtos_tcb_t *list_pop_head(rtos_tcb_t **head);

// Return the head without removing it.
rtos_tcb_t *list_peek_head(rtos_tcb_t *head);

#endif // RTOS_LIST_H
