//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Internal priority-sorted linked list.
//---------------------------------------------------------------------------
#include "list.h"
#include <stddef.h>

//---------------------------------------------------------------------------
// Insert tcb into the list headed by *head, sorted ascending by key.
// Lower key values are closer to the head.
// Used for the blocked list (key = delay_ticks: soonest wakeup first) and
// for IPC wait lists (key = priority: highest-priority waiter = lowest number first).
//---------------------------------------------------------------------------
void list_insert_sorted(rtos_tcb_t **head, rtos_tcb_t *tcb, uint32_t key)
{
    tcb->sort_key = key;
    tcb->next = NULL;

    if (!*head || key < (*head)->sort_key) {
        tcb->next = *head;
        *head = tcb;
        return;
    }

    rtos_tcb_t *cur = *head;
    while (cur->next && cur->next->sort_key <= key)
        cur = cur->next;

    tcb->next = cur->next;
    cur->next = tcb;
}

//---------------------------------------------------------------------------
// Insert at the tail (for FIFO within same priority).
//---------------------------------------------------------------------------
void list_insert_tail(rtos_tcb_t **head, rtos_tcb_t *tcb)
{
    tcb->next = NULL;
    
    if (!*head) 
    {
        *head = tcb;
        return;
    }

    rtos_tcb_t *cur = *head;

    while (cur->next)
        cur = cur->next;

        cur->next = tcb;
}

//---------------------------------------------------------------------------
// Remove a specific tcb from the list. Returns 1 if found and removed.
//---------------------------------------------------------------------------
int list_remove(rtos_tcb_t **head, rtos_tcb_t *tcb)
{
    if (!*head) return 0;

    if (*head == tcb) 
    {
        *head = tcb->next;
        tcb->next = NULL;
        return 1;
    }

    rtos_tcb_t *cur = *head;

    while (cur->next) 
    {
        if (cur->next == tcb) 
        {
            cur->next = tcb->next;
            tcb->next = NULL;
            return 1;
        }
        cur = cur->next;
    }

    return 0;
}

//---------------------------------------------------------------------------
// Remove and return the head (highest priority / soonest deadline).
//---------------------------------------------------------------------------
rtos_tcb_t *list_pop_head(rtos_tcb_t **head)
{
    if (!*head) return NULL;

    rtos_tcb_t *tcb = *head;
    *head = tcb->next;
    tcb->next = NULL;

    return tcb;
}

//---------------------------------------------------------------------------
// Return the head without removing it.
//---------------------------------------------------------------------------
rtos_tcb_t *list_peek_head(rtos_tcb_t *head)
{
    return head;
}
