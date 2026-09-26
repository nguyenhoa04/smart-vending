#ifndef PI_UART_H
#define PI_UART_H

#include "driver/uart.h"
#include "driver/gpio.h"


#define UART_PORT_NUM      UART_NUM_1
#define UART_BAUD_RATE     115200
#define UART_TX_PIN        GPIO_NUM_17
#define UART_RX_PIN        GPIO_NUM_16
#define BUF_SIZE           1024

void init_uart(void);

void send_to_pi(const char* data);

#endif