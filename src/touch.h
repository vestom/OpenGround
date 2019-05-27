/*
    Copyright 2016 fishpepper <AT> gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    author: fishpepper <AT> gmail.com
*/

#ifndef TOUCH_H_
#define TOUCH_H_

#include <stdint.h>

#include "config.h"

void touch_init(void);
void touch_test(void);


typedef struct {
    uint8_t event_id;
    uint16_t x;
    uint16_t y;
} touch_event_t;

touch_event_t touch_get_and_clear_last_event(void);

// void EXTI4_15_IRQHandler(void);

#define TOUCH_FT6236_MAX_TOUCH_POINTS     2

#define TOUCH_FT6236_REG_TH_GROUP         0x80
#define TOUCH_FT6236_REG_PERIODACTIVE     0x88
#define TOUCH_FT6236_REG_LIB_VER_H        0xa1
#define TOUCH_FT6236_REG_LIB_VER_L        0xa2
#define TOUCH_FT6236_REG_CIPHER           0xa3
#define TOUCH_FT6236_REG_FIRMID           0xa6
#define TOUCH_FT6236_REG_FOCALTECH_ID     0xa8
#define TOUCH_FT6236_REG_RELEASE_CODE_ID  0xaf

#define TOUCH_FT6236_EVENT_PRESS_DOWN     0
#define TOUCH_FT6236_EVENT_LIFT_UP        1
#define TOUCH_FT6236_EVENT_CONTACT        2
#define TOUCH_FT6236_EVENT_NO_EVENT       3

#define TOUCH_RESET_HI() { gpio_set(TOUCH_RESET_GPIO, TOUCH_RESET_PIN); }
#define TOUCH_RESET_LO() { gpio_clear(TOUCH_RESET_GPIO, TOUCH_RESET_PIN); }

#define TOUCH_FT6236_GESTURE_MOVE_FLAG   0x10
#define TOUCH_FT6236_GESTURE_MOVE_UP     0x10
#define TOUCH_FT6236_GESTURE_MOVE_RIGHT  0x14
#define TOUCH_FT6236_GESTURE_MOVE_DOWN   0x18
#define TOUCH_FT6236_GESTURE_MOVE_LEFT   0x1C
#define TOUCH_FT6236_GESTURE_ZOOM_IN     0x48
#define TOUCH_FT6236_GESTURE_ZOOM_OUT    0x49
#define TOUCH_FT6236_GESTURE_NONE        0x00

#define TOUCH_GESTURE_UP    ((TOUCH_FT6236_GESTURE_MOVE_UP    & 0x0F)+1)
#define TOUCH_GESTURE_DOWN  ((TOUCH_FT6236_GESTURE_MOVE_DOWN  & 0x0F)+1)
#define TOUCH_GESTURE_LEFT  ((TOUCH_FT6236_GESTURE_MOVE_LEFT  & 0x0F)+1)
#define TOUCH_GESTURE_RIGHT ((TOUCH_FT6236_GESTURE_MOVE_RIGHT & 0x0F)+1)
#define TOUCH_GESTURE_MOUSE_DOWN (0x80+TOUCH_FT6236_EVENT_PRESS_DOWN)
#define TOUCH_GESTURE_MOUSE_UP   (0x80+TOUCH_FT6236_EVENT_LIFT_UP)
#define TOUCH_GESTURE_MOUSE_MOVE (0x80+TOUCH_FT6236_EVENT_CONTACT)
#define TOUCH_GESTURE_MOUSE_NONE (0x80+TOUCH_FT6236_EVENT_NO_EVENT)

#define I2C_CR2_FREQ_MASK       0x3ff
#define I2C_CCR_CCRMASK         0xfff
#define I2C_TRISE_MASK          0x3f

struct __attribute__((__packed__)) touch_ft6236_touchpoint {
    union {
        uint8_t xhi;
        uint8_t event;
    };

    uint8_t xlo;

    union {
        uint8_t yhi;
        uint8_t id;
    };

    uint8_t ylo;
    uint8_t weight;
    uint8_t misc;
};

// this packet represents the register map as read from offset 0
typedef struct __attribute__((__packed__)) {
    uint8_t dev_mode;
    uint8_t gest_id;
    uint8_t touches;
    struct touch_ft6236_touchpoint points[TOUCH_FT6236_MAX_TOUCH_POINTS];
} touch_ft6236_packet_t;

touch_event_t touch_get_last_event(void);

#endif  // TOUCH_H_
