/* Original code Copyright (C) 2015 Baruch Even
 *
 * Modifications 2020 Kristian Wiklund, this file created by splitting out serial interface to a separate file
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
#include "version.h"
#include "stm8s.h"
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "display.h"
#include "fixedpoint.h"
#include "uart.h"
#include "eeprom.h"
#include "outputs.h"
#include "config.h"
#include "parse.h"
#include "adc.h"

#include "capabilities.h"

extern cfg_system_t cfg_system;
extern cfg_output_t cfg_output;
extern state_t state;

void set_name(uint8_t *name)
{
	uint8_t idx;

	for (idx = 0; name[idx] != 0; idx++) {
		if (!isprint(name[idx]))
			name[idx] = '.'; // Eliminate non-printable chars
	}

	memcpy(cfg_system.name, name, sizeof(cfg_system.name));
	cfg_system.name[sizeof(cfg_system.name)-1] = 0;

	uart_write_str("SNAME: ");
	uart_write_str(cfg_system.name);
	uart_write_str("\r\n");
}

void autocommit(void)
{
	if (cfg_system.autocommit) {
		commit_output();
	} else {
		uart_write_str("AUTOCOMMIT OFF: CHANGE PENDING ON COMMIT\r\n");
	}
}

void set_output(uint8_t *s)
{
	if (s[1] != 0) {
		uart_write_str("OUTPUT takes either 0 for OFF or 1 for ON, received: \"");
		uart_write_str(s);
		uart_write_str("\"\r\n");
		return;
	}

	if (s[0] == '0') {
		cfg_system.output = 0;
		uart_write_str("OUTPUT: OFF\r\n");
	} else if (s[0] == '1') {
		cfg_system.output = 1;
		uart_write_str("OUTPUT: ON\r\n");
	} else {
		uart_write_str("OUTPUT takes either 0 for OFF or 1 for ON, received: \"");
		uart_write_str(s);
		uart_write_str("\"\r\n");
	}

	autocommit();
}

void set_voltage(uint8_t *s)
{
	fixed_t val;

	val = parse_millinum(s);
	if (val == 0xFFFF)
		return;

	if (val > CAP_VMAX) {
		uart_write_str("VOLTAGE VALUE TOO HIGH\r\n");
		return;
	}
	if (val < CAP_VMIN) {
		uart_write_str("VOLTAGE VALUE TOO LOW\r\n");
		return;
	}

	uart_write_str("VOLTAGE: SET ");
	uart_write_millivolt(val);
	uart_write_str("\r\n");
	cfg_output.vset = val;

	autocommit();
}

void set_current(uint8_t *s)
{
	fixed_t val;

	val = parse_millinum(s);
	if (val == 0xFFFF)
		return;

	if (val > CAP_CMAX) {
		uart_write_str("CURRENT VALUE TOO HIGH\r\n");
		return;
	}
	if (val < CAP_CMIN) {
		uart_write_str("CURRENT VALUE TOO LOW\r\n");
		return;
	}

	uart_write_str("CURRENT: SET ");
	uart_write_milliamp(val);
	uart_write_str("\r\n");
	cfg_output.cset = val;

	autocommit();
}

void set_autocommit(uint8_t *s)
{
	if (strcmp(s, "1") == 0 || strcmp(s, "YES") == 0) {
		cfg_system.autocommit = 1;
		uart_write_str("AUTOCOMMIT: YES\r\n");
	} else if (strcmp(s, "0") == 0 || strcmp(s, "NO") == 0) {
		cfg_system.autocommit = 0;
		uart_write_str("AUTOCOMMIT: NO\r\n");
	} else {
		uart_write_str("UNKNOWN AUTOCOMMIT ARG: ");
		uart_write_str(s);
		uart_write_str("\r\n");
	}
}

void write_str(const char *prefix, const char *val)
{
	uart_write_str(prefix);
	uart_write_str(val);
	uart_write_str("\r\n");
}

void write_onoff(const char *prefix, uint8_t on)
{
	write_str(prefix, on ? "ON" : "OFF");
}

void write_millivolt(const char *prefix, uint16_t mv)
{
	uart_write_str(prefix);
	uart_write_millivolt(mv);
	uart_write_str("\r\n");
}

void write_milliamp(const char *prefix, uint16_t ma)
{
	uart_write_str(prefix);
	uart_write_milliamp(ma);
	uart_write_str("\r\n");
}

void write_int(const char *prefix, uint16_t val)
{
	uart_write_str(prefix);
	uart_write_int(val);
	uart_write_str("\r\n");
}

uint32_t _parse_uint(uint8_t *s)
{
	uint32_t val = 0;

	for (; *s; s++) {
		uint8_t ch = *s;
		if (ch >= '0' && ch <= '9') {
			val = val*10 + (ch-'0');
		} else {
			return 0xFFFFFFFF;
		}
	}

	return val;
}

void parse_uint(const char *name, uint32_t *pval, uint8_t *s)
{
	uint32_t val = _parse_uint(s);
	if (val == 0xFFFFFFFF) {
		uart_write_str("FAILED TO PARSE ");
		uart_write_str(s);
		uart_write_str(" FOR ");
		uart_write_str(name);
	} else {
		*pval = val;
		uart_write_str("CALIBRATION SET ");
		uart_write_str(name);
	}
	uart_write_str("\r\n");
}

#define uws(x) uart_write_str(x"\r")
#define command(c,w) if(!strcmp(c,urb)) {w;  uart_read_len = 0;return;}
		
void process_input()
{
  char *urb;
  // Eliminate the CR/LF character
  //uart_read_buf[uart_read_len-1] = 0;
  urb = uart_read_buf;
  
  // uart_read_buf contains the entire string.
  // we want to implement primarily this: https://sigrok.org/wiki/Korad_KAxxxxP_series#Protocol

  command("*IDN?", uws(MODEL));
  command("VSET1?", uart_write_millivolt(cfg_output.vset));
  command("VOUT1?", uart_write_millivolt(state.vout));
  command("IOUT1?", uart_write_milliamp(state.cout));
  command("ISET1?", uart_write_milliamp(cfg_output.cset));
  command("OUT1", set_output("1"));
  command("OUT0", set_output("0"));

  // if we get here, we haven't had any match.

  //read_newline = 0;
}
