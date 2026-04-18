// Copyright 2025. All rights reserved.
// Internal priority-sorted linked list.
#include "list.h"
#include <stddef.h>

void list_insert_sorted(rtos_tcb_t **head, rtos_tcb_t *tcb, uint32_t key)
{
    (void)key;  // placeholder until scheduler uses it
    list_insert_tail(head, tcb);
}

void list_insert_tail(rtos_tcb_t **head, rtos_tcb_t *tcb)
{
    tcb->next = NULL;
    if (!*head) {
        *head = tcb;
        return;
    }
    rtos_tcb_t *cur = *head;
    while (cur->next)
        cur = cur->next;
    cur->next = tcb;
}

int list_remove(rtos_tcb_t **head, rtos_tcb_t *tcb)
{
    if (!*head) return 0;
    if (*head == tcb) {
        *head = tcb->next;
        tcb->next = NULL;
        return 1;
    }
    rtos_tcb_t *cur = *head;
    while (cur->next) {
        if (cur->next == tcb) {
            cur->next = tcb->next;
            tcb->next = NULL;
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

rtos_tcb_t *list_pop_head(rtos_tcb_t **head)
{
    if (!*head) return NULL;
    rtos_tcb_t *tcb = *head;
    *head = tcb->next;
    tcb->next = NULL;
    return tcb;
}

rtos_tcb_t *list_peek_head(rtos_tcb_t *head)
{
    return head;
}
