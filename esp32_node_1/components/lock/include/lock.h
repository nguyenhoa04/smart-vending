#ifndef LOCK_H
#define LOCK_H

#include <stdbool.h>
#include "hal/gpio_types.h"

#define DOOR_SENSOR_PIN GPIO_NUM_5

void lock_init(void);
void lock_open(void);
void lock_close(void);

void door_sensor_init(void);
bool is_door_closed(void);

#endif // LOCK_H