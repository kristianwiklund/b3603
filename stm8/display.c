/* Original code copyright (C) 2015 Baruch Even
 * Additional code by Arjan Filius (copied manually from https://github.com/iafilius/b3603)
 * Additional code by Kristian Wiklund
 * 
 * This file is part of the B3603 alternative firmware.
 *
 *  B3603 alternative firmware is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  B3603 alternative firmware is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with B3603 alternative firmware.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "display.h"
#include "stm8s.h"

#include <string.h>

uint8_t display_idx;
uint8_t display_data[4];
uint8_t pending_display_data[4];
uint8_t pending_update;
uint16_t timer;

static const uint8_t display_number[10] = {
	0xFC, // '0'
	0x60, // '1'
	0xDA, // '2'
	0xF2, // '3'
	0x66, // '4'
	0xB6, // '5'
	0xBE, // '6'
	0xE0, // '7'
	0xFE, // '8'
	0xF6, // '9'
};

// repeated integer calculations to char 
#define D4	10000
#define D3	1000
#define D2	100
#define D1	10

#define SET_DATA(bit) do { if (bit) { PD_ODR |= (1<<4); } else { PD_ODR &= ~(1<<4); }} while (0)
#define PULSE_CLOCK() do { PA_ODR |= (1<<1); PA_ODR &= ~(1<<1); } while (0)
#define SAVE_DATA() do { PA_ODR &= ~(1<<2); PA_ODR |= (1<<2); } while (0)



inline void display_word(uint16_t word)
{
	uint8_t i;

	for (i = 0; i < 16; i++) {
		uint8_t bit = word & 1;
		word >>= 1;
		SET_DATA(bit);
		PULSE_CLOCK();
	}
	SAVE_DATA();
}

void display_refresh(void)
{
	uint8_t i = display_idx++;
	uint8_t bit = 8+(i*2);
	uint16_t digit = 0xFF00 ^ (3<<bit);
	
	//uint16_t digit=(~(0x03<<(2*i)))<<8;
	//uint8_t digit[4]={0xFC,0xF3,0xCF,0x3F};

	if (timer > 0)
		timer--;
	
	if (pending_update && timer == 0) {
		memcpy(display_data, pending_display_data, sizeof(display_data));
		pending_update = 0;
		timer = 1500; // 1/2 of a second, approximately
	}
	

	display_word(digit | display_data[i]);

	if (display_idx == 4)
		display_idx = 0;
}


uint8_t display_char(uint8_t ch, uint8_t dot)
{
	if (dot)
		dot = 1;
	if (ch >= '0' && ch <= '9')
		return display_number[ch-'0'] | dot;
	return dot;
}

void display_show_raw_digits(uint8_t ch1, uint8_t ch2, uint8_t ch3, uint8_t ch4 )
{
	pending_display_data[3] = ch1;
	pending_display_data[2] = ch2;
	pending_display_data[1] = ch3;
	pending_display_data[0] = ch4;
	pending_update = 1;
}

void display_show(uint8_t ch1, uint8_t dot1, uint8_t ch2, uint8_t dot2, uint8_t ch3, uint8_t dot3, uint8_t ch4, uint8_t dot4)
{
  /*	pending_display_data[3] = display_char(ch1, dot1);
	pending_display_data[2] = display_char(ch2, dot2);
	pending_display_data[1] = display_char(ch3, dot3);
	pending_display_data[0] = display_char(ch4, dot4);*/

  display_show_raw_digits(display_char(ch1,dot1),
			  display_char(ch2,dot2),
			  display_char(ch3,dot3),
			  display_char(ch4,dot4));
}

uint8_t uint16_to_digit(uint16_t value, uint16_t devider)
{
  return '0'+(value / devider) % 10;
}

// Displays the cfg_output_t uint16_t values
void display_show_uint16(uint16_t value)
{
uint8_t ch1;
uint8_t ch2;
uint8_t ch3;
uint8_t ch4;

    ch1 = uint16_to_digit(value, D4);

    ch2 = uint16_to_digit(value, D3);

    ch3 = uint16_to_digit(value, D2);
                
    ch4 = uint16_to_digit(value, D1);

    display_show(ch1, 0, ch2, 1, ch3, 0, ch4, 0);
}
