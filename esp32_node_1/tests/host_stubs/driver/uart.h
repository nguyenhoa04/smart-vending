#pragma once
#include <stdint.h>
#define UART_NUM_1 1
int uart_read_bytes(int port, void *data, uint32_t size, uint32_t timeout);
