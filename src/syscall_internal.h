#pragma once

#include "syscall.h"

void syscall_uaccess_ensure_user_cr3(void);
void poll_network_once(void);
uint32_t *fd_refcount_create(void);
void kernel_socket_destroy(kernel_socket_t *sock);
