/* File: main.cpp
 * Contains base main function and usually all the other stuff that avr does...
 */
/* Copyright (c) 2012-2013 Domen Ipavec (domen.ipavec@z-v.si)
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <util/delay.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
//#include <avr/eeprom.h> 

#include <stdint.h>

#include "bitop.h"
#include "random32.h"

static volatile uint8_t intensity_buffer[64];

static uint16_t buffer[8][4];

static uint8_t usart_timeout = 0;

static uint8_t on = 1;

static uint8_t animation_state = 0;
static uint8_t animation_id = 0;

static const uint8_t ANIMATIONS = 3;

static void animation1() {
    for (uint8_t layer = 0; layer < 4; layer++) {
        for (uint8_t row = 0; row < 4; row++) {
            uint8_t ccolumn = animation_state;
            if (animation_state >= 7) {
                ccolumn -= 7;
                if (ccolumn >= 4) {
                    ccolumn = 6-ccolumn;
                }
                for (uint8_t column = 0; column < 4; column++) {
                    if (column == ccolumn) {
                        intensity_buffer[layer*16 + row + column*4] = 255;
                    } else {
                        intensity_buffer[layer*16 + row + column*4] = 0;
                    }
                }
            } else {
                if (ccolumn >= 4) {
                    ccolumn = 6-ccolumn;
                }
                for (uint8_t column = 0; column < 4; column++) {
                    if (column == ccolumn) {
                        intensity_buffer[layer*16 + row*4 + column] = 255;
                    } else {
                        intensity_buffer[layer*16 + row*4 + column] = 0;
                    }
                }
            }
        }
    }
    animation_state++;
    if (animation_state >= 14) {
        animation_state = 0;
    }
}

static void animation2() {
    for (uint8_t layer = 0; layer < 4; layer++) {
        if (animation_state == 0) {
            intensity_buffer[layer*16 + 15] = 0;
        } else {
            intensity_buffer[layer*16 + animation_state - 1] = 0;
        }
        intensity_buffer[layer*16 + animation_state] = 255;
    };
    animation_state++;
    if (animation_state >= 16) {
        animation_state = 0;
    }
}

static void animation3() {
    for (uint8_t i = 0; i < 64; i++) {
        if (get_random32(4)) {
            intensity_buffer[i] = 0;
        } else {
            intensity_buffer[i] = 255;
        }
    }
}

static void animation4() {
    for (uint8_t i = 0; i < 64; i++) {
        intensity_buffer[i] = 255;
    }
}

static void (*animations[])() = {
    &animation3,
    &animation1,
    &animation2,
    &animation4
};

static const uint8_t layer_mosfet[4] = {
    PA6,
    PA4,
    PA5,
    PA3
};

// map xy to tlc5925 ports
static const uint8_t position_map[16] = {
    11, 8, 7, 4,
    12, 9, 6, 3,
    13, 10, 5, 2,
    14, 15, 0, 1
};

static inline void recalculate(uint8_t layer) {
    for (uint8_t power_bit = 0; power_bit < 8; power_bit++) {
        buffer[power_bit][layer] = 0;
    }
    for (uint8_t position = 0; position < 16; position++) {
        uint8_t power_value = intensity_buffer[layer*16 + position];
        for (uint8_t power_bit = 0; power_bit < 8; power_bit++) {
            buffer[power_bit][layer] |= ((power_value >> power_bit) & 1) << position_map[position];
        }
    }
}

static inline void shift_out_data(uint8_t layer, uint8_t power_bit) {
    uint8_t porta_buf = PORTA;
    uint16_t data = buffer[power_bit][layer];
    for (uint8_t i = 16; i != 0; --i) {
        PORTA = porta_buf | (data & 1);
        data >>= 1;
        SETBIT(PORTA, PA7);
    }
    PORTA = porta_buf;
}

static inline void change_layer(uint8_t layer) {
    // turn previous layer off
    if (layer == 0) {
        SETBIT(PORTA, layer_mosfet[3]);
    } else {
        SETBIT(PORTA, layer_mosfet[layer-1]);
    }
    
    // latch
    SETBIT(PORTB, PB2);
    CLEARBIT(PORTB, PB2);
    
    // turn next layer on
    CLEARBIT(PORTA, layer_mosfet[layer]);
}

static inline void wait_for_timer(uint8_t layer, uint8_t bit) {
    while (TCNT1 < 4*((1<<bit)-1)+(1<<bit)*layer);
}

static inline void animation() {
    static uint8_t delay = 0;
    
    delay++;
    if (delay == 20) {
        delay = 0;
        
        animations[animation_id]();
    }
};

static inline void logic(uint8_t layer) {

    static uint8_t button_timeout = 255;
    switch (layer) {
        case 0:
            if (usart_timeout > 0) {
                usart_timeout--;
            }
            break;
        case 1:
            if (usart_timeout == 0 && on) {
                animation();
            }
            break;
        case 2:
            if (BITCLEAR(PINB, PB1)) {
                if (button_timeout > 0) {
                    button_timeout--;
                }
                if (on) {
                    // shutdown on long button press
                    if (button_timeout == 0) {
                        on = 0;
                    }
                    
                    // change animation on short press
                    if (button_timeout == 250) {
                        // clear
                        for (uint8_t i = 0; i < 64; i++) {
                            intensity_buffer[i] = 0;
                        }
    
                        animation_id++;
                        if (animation_id > ANIMATIONS) {
                            animation_id = 0;
                        }
                    }
                } else {
                    if (button_timeout == 200) {
                        on = 1;
                    }
                }
            } else {
                button_timeout = 255;
            }
            break;
    }
}

int main() {
	// init
	
    // set outputs for TLC5925
    SETBITS(DDRA, BIT(PA0) | BIT(PA7));
    SETBIT(DDRB, PB2);
    
    // set outputs for mosfets
    SETBITS(PORTA, BIT(PA3) | BIT(PA4) | BIT(PA5) | BIT(PA6));
    SETBITS(DDRA, BIT(PA3) | BIT(PA4) | BIT(PA5) | BIT(PA6));
    
    // set button pull up
    SETBIT(PUEB, PB1);

    // init timer 1 to clk/256
    TCCR1B = 0b00001100;
    OCR1A = 511;

    // init uart
    UCSR0B = 0b10010000;
    UCSR0C = 0b00000110;
    UBRR0 = 25;
    // for 12 Mhz
    //UBRR0 = 38; // baud 19200
    
    // intensity to 0
    for (uint8_t i = 0; i < 64; i++) {
        intensity_buffer[i] = 0;
    }
    
    // recalculate data
    for (uint8_t layer = 0; layer < 4; ++layer) {
        recalculate(layer);
    }
    
    // enable interrupts
    sei();

    // init timer 1 to 1023
    TCNT1 = OCR1A - 1;
    
    for (;;) {
        if (on) {
            // bit 0
            cli();
            for (uint8_t layer = 0; layer < 4; ++layer) {
                shift_out_data(layer, 0);
                
                if (layer == 0) {
                    while (TCNT1 != 0);
                } else {
                    while (TCNT1 < layer);
                }
                
                change_layer(layer);
            }
            sei();
            
            // other bits
            for (uint8_t bit = 1; bit < 6; bit++) {
                for (uint8_t layer = 0; layer < 4; ++layer) {
                    shift_out_data(layer, bit);
                    
                    wait_for_timer(layer, bit);
                    
                    change_layer(layer);
                } 
            }
            
            // bit 6
            for (uint8_t layer = 0; layer < 4; ++layer) {
                shift_out_data(layer, 6);
                
                wait_for_timer(layer, 6);
                
                change_layer(layer);
                
                recalculate(layer);
                
                logic(layer);
            }
        } else {
            SETBITS(PORTA, BIT(PA3) | BIT(PA4) | BIT(PA5) | BIT(PA6));
            if (TCNT1 == 0) {
                logic(2); 
            }
        }
    }
}

ISR(USART0_RX_vect) {
    static uint8_t counter = 0;
    
    uint8_t data = UDR0;
    
    if (BITSET(data, 7)) {
        if (BITSET(data, 6)) {
            // set position
            counter = data & 0b00111111;
        } else {
            switch (data & 0b00111111) {
                // go home
                case 0:
                    counter = 0;
                    break;
                // off
                case 1:
                    on = 0;
                    break;
                // on
                case 2:
                    on = 1;
                    break;
                // change animation
                case 3:
                    animation_id++;
                    if (animation_id > ANIMATIONS) {
                        animation_id = 0;
                    }
                // clear
                case 4:
                    for (uint8_t i = 0; i < 64; i++) {
                        intensity_buffer[i] = 0;
                    }
                    break;
                // ping
                case 5:
                    usart_timeout = 255;
                    break;
            }
        }
    } else {
        // data
        usart_timeout = 255;
        intensity_buffer[counter] = data;
        counter++;
        if (counter == 64) {
            counter = 0;
        }
    }
}