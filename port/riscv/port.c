// Copyright 2025. All rights reserved.
// RISC-V port stub — to be implemented.
// Placeholder so the build system can reference the file.
#include "../../src/port.h"

void port_enter_critical(void)     { /* TODO: csrci mstatus, 0x8 */ }
void port_exit_critical(void)      { /* TODO: csrsi mstatus, 0x8 */ }
void port_request_reschedule(void) { /* TODO: set software interrupt pending */ }
void port_init(uint32_t hz)        { (void)hz; /* TODO: MTIME setup */ }
void port_start_first_task(void)   { /* TODO */ }

uint32_t *port_init_stack(uint32_t *stack_top,
                           void    (*func)(void *),
                           void     *arg)
{
    (void)func; (void)arg;
    return stack_top; // TODO
}

