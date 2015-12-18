/* Copyright (C) 2015 Baruch Even
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

#define FW_VERSION "1.0.1"
#define MODEL "B3603"

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

#define CAP_VMIN 10 // 10mV
#define CAP_VMAX 35000 // 35 V
#define CAP_VSTEP 10 // 10mV

#define CAP_CMIN 1 // 1 mA
#define CAP_CMAX 3000 // 3 A
#define CAP_CSTEP 1 // 1 mA

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static cfg_system_t cfg_system;
static cfg_output_t cfg_output;
static state_t state;

inline void iwatchdog_init(void)
{
	IWDG_KR = 0xCC; // Enable IWDG
	// The default values give us about 15msec between pings
}

inline void iwatchdog_tick(void)
{
	IWDG_KR = 0xAA; // Reset the counter
}

static void write_str(const char *prefix, const char *val)
{
	uart_write_str(prefix);
	uart_write_str(val);
	uart_write_str("\r\n");
}

static void write_onoff(const char *prefix, uint8_t on)
{
	write_str(prefix, on ? "ON" : "OFF");
}

static void write_millis(const char *prefix, uint16_t val)
{
	uart_write_str(prefix);
	uart_write_millis(val);
	uart_write_str("\r\n");
}

static void write_uint(const char *prefix, uint32_t val)
{
	uart_write_str(prefix);
	uart_write_uint(val);
	uart_write_str("\r\n");
}

static void write_calibration_fixed_point(const char* prefix, calibrate_t *cal)
{
	uart_write_str("CALIBRATE ");
	uart_write_str(prefix);
	uart_write_fixed_point(cal->a);
	uart_write_ch('/');
	uart_write_fixed_point(cal->b);
	uart_write_str("\r\n");
}

static void write_calibration_uint(const char* prefix, calibrate_t *cal)
{
	uart_write_str("CALIBRATE ");
	uart_write_str(prefix);
	uart_write_uint(cal->a);
	uart_write_ch('/');
	uart_write_uint(cal->b);
	uart_write_str("\r\n");
}

static void commit_output()
{
	output_commit(&cfg_output, &cfg_system, state.constant_current);
}

static void autocommit(void)
{
	if (cfg_system.autocommit) {
		commit_output();
	} else {
		uart_write_str("AUTOCOMMIT OFF: CHANGE PENDING ON COMMIT\r\n");
	}
}

typedef struct command {
	const char *name;
	void (*handler)(uint8_t *data);
	uint8_t argc;
} command_t;

static void cmd_sname(uint8_t *name)
{
	uint8_t idx;

	for (idx = 0; name[idx] != 0; idx++) {
		if (!isprint(name[idx]))
			name[idx] = '.'; // Eliminate non-printable chars
	}

	memcpy(cfg_system.name, name, sizeof(cfg_system.name));
	cfg_system.name[sizeof(cfg_system.name)-1] = 0;

	write_str("SNAME: ", cfg_system.name);
}

static void cmd_output(uint8_t *s)
{
	if ((s[0] == '0' || s[0] == '1') && s[1] == 0) {
		cfg_system.output = s[0] - '0';
		write_onoff("OUTPUT: ", cfg_system.output);
		autocommit();
	} else {
		uart_write_str("OUTPUT takes either 0 for OFF or 1 for ON, received: \"");
		uart_write_str(s);
		uart_write_str("\"\r\n");
	}
}

static void cmd_voltage(uint8_t *s)
{
	fixed_t val;

	val = parse_millinum(s);
	if (val == 0xFFFF)
		return;

	if (val > CAP_VMAX) {
		uart_write_str("VOLTAGE");
		uart_write_str(" VALUE TOO HIGH\r\n");
		return;
	}
	if (val < CAP_VMIN) {
		uart_write_str("VOLTAGE");
		uart_write_str(" VALUE TOO LOW\r\n");
		return;
	}

	write_millis("VOLTAGE: SET ", val);
	cfg_output.vset = val;

	autocommit();
}

static void cmd_current(uint8_t *s)
{
	fixed_t val;

	val = parse_millinum(s);
	if (val == 0xFFFF)
		return;

	if (val > CAP_CMAX) {
		uart_write_str("CURRENT");
		uart_write_str(" VALUE TOO HIGH\r\n");
		return;
	}
	if (val < CAP_CMIN) {
		uart_write_str("CURRENT");
		uart_write_str(" VALUE TOO LOW\r\n");
		return;
	}

	write_millis("CURRENT: SET ", val);
	cfg_output.cset = val;

	autocommit();
}

static void cmd_autocommit(uint8_t *s)
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

inline uint32_t _parse_uint(uint8_t *s)
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

static void parse_uint(const char *name, uint32_t *pval, uint8_t *s)
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

#define CMD_CAL_WRAPPER(name, text, var) \
	static void name(uint8_t *data) \
	{ \
		parse_uint(text, var, data); \
	}

CMD_CAL_WRAPPER(cmd_cal_vin_adc_a, "VIN ADC A", &cfg_system.vin_adc.a)
CMD_CAL_WRAPPER(cmd_cal_vin_adc_b, "VIN ADC B", &cfg_system.vin_adc.b)
CMD_CAL_WRAPPER(cmd_cal_vout_adc_a, "VOUT ADC A", &cfg_system.vout_adc.a)
CMD_CAL_WRAPPER(cmd_cal_vout_adc_b, "VOUT ADC B", &cfg_system.vout_adc.b)
CMD_CAL_WRAPPER(cmd_cal_vout_pwm_a, "VOUT PWM A", &cfg_system.vout_pwm.a)
CMD_CAL_WRAPPER(cmd_cal_vout_pwm_b, "VOUT PWM B", &cfg_system.vout_pwm.b)
CMD_CAL_WRAPPER(cmd_cal_cout_adc_a, "COUT ADC A", &cfg_system.cout_adc.a)
CMD_CAL_WRAPPER(cmd_cal_cout_adc_b, "COUT ADC B", &cfg_system.cout_adc.b)
CMD_CAL_WRAPPER(cmd_cal_cout_pwm_a, "COUT PWM A", &cfg_system.cout_pwm.a)
CMD_CAL_WRAPPER(cmd_cal_cout_pwm_b, "COUT PWM B", &cfg_system.cout_pwm.b)

#undef CMD_CAL_WRAPPER

static void cmd_model(uint8_t *data)
{
	(void) data;

	uart_write_str("MODEL: " MODEL "\r\n");
}

static void cmd_version(uint8_t *data)
{
	(void) data;

	uart_write_str("VERSION: " FW_VERSION "\r\n");
}

static void cmd_system(uint8_t *data)
{
	(void) data;

	cmd_model(NULL);
	cmd_version(NULL);

	write_str("NAME: ", cfg_system.name);
	write_onoff("ONSTARTUP: ", cfg_system.default_on);
	write_onoff("AUTOCOMMIT: ", cfg_system.autocommit);
}

static void cmd_calibration(uint8_t *data)
{
	(void) data;

	write_calibration_fixed_point("VIN ADC: ", &cfg_system.vin_adc);
	write_calibration_fixed_point("VOUT ADC: ", &cfg_system.vout_adc);
	write_calibration_fixed_point("COUT ADC: ", &cfg_system.cout_adc);
	write_calibration_fixed_point("VOUT PWM: ", &cfg_system.vout_pwm);
	write_calibration_fixed_point("COUT PWM: ", &cfg_system.cout_pwm);
}

static void cmd_rcalibration(uint8_t *data)
{
	(void) data;

	write_calibration_uint("VIN ADC: ", &cfg_system.vin_adc);
	write_calibration_uint("VOUT ADC: ", &cfg_system.vout_adc);
	write_calibration_uint("COUT ADC: ", &cfg_system.cout_adc);
	write_calibration_uint("VOUT PWM: ", &cfg_system.vout_pwm);
	write_calibration_uint("COUT PWM: ", &cfg_system.cout_pwm);
}

static void cmd_limits(uint8_t *data)
{
	(void) data;

	uart_write_str("LIMITS:\r\n");
	write_millis("VMIN: ", CAP_VMIN);
	write_millis("VMAX: ", CAP_VMAX);
	write_millis("VSTEP: ", CAP_VSTEP);
	write_millis("CMIN: ", CAP_CMIN);
	write_millis("CMAX: ", CAP_CMAX);
	write_millis("CSTEP: ", CAP_CSTEP);
}

static void cmd_config(uint8_t *data)
{
	(void) data;

	uart_write_str("CONFIG:\r\n");
	write_onoff("OUTPUT: ", cfg_system.output);
	write_millis("VSET: ", cfg_output.vset);
	write_millis("CSET: ", cfg_output.cset);
	write_millis("VSHUTDOWN: ", cfg_output.vshutdown);
	write_millis("CSHUTDOWN: ", cfg_output.cshutdown);
}

static void cmd_status(uint8_t *data)
{
	(void) data;

	uart_write_str("STATUS:\r\n");
	write_onoff("OUTPUT: ", cfg_system.output);
	write_millis("VIN: ", state.vin);
	write_millis("VOUT: ", state.vout);
	write_millis("COUT: ", state.cout);
	write_str("CONSTANT: ", state.constant_current ? "CURRENT" : "VOLTAGE");
}

static void cmd_rstatus(uint8_t *data)
{
	(void) data;

	uart_write_str("RSTATUS:\r\n");
	write_onoff("OUTPUT: ", cfg_system.output);
	write_uint("VIN ADC: ", state.vin_raw);
	write_millis("VIN: ", state.vin);
	write_uint("VOUT ADC: ", state.vout_raw);
	write_millis("VOUT: ", state.vout);
	write_uint("COUT ADC: ", state.cout_raw);
	write_millis("COUT: ", state.cout);
	write_str("CONSTANT: ", state.constant_current ? "CURRENT" : "VOLTAGE");
}

static void cmd_commit(uint8_t *data)
{
	(void) data;

	commit_output();
}

static void cmd_save(uint8_t *data)
{
	(void) data;

	config_save_system(&cfg_system);
	config_save_output(&cfg_output);
	uart_write_str("SAVED\r\n");
}

static void cmd_load(uint8_t *data)
{
	(void) data;

	config_load_system(&cfg_system);
	config_load_output(&cfg_output);
	autocommit();
}

static void cmd_restore(uint8_t *data)
{
	(void) data;

	config_default_system(&cfg_system);
	config_default_output(&cfg_output);
	autocommit();
}

#if DEBUG
static void cmd_stuck(uint8_t *data)
{
	(void) data;

	// Allows debugging of the IWDG feature
	uart_write_str("STUCK\r\n");
	uart_write_flush();
	while(1); // Induce watchdog reset
}
#endif

static const command_t commands[] = {
	{ .name = "MODEL", .handler = cmd_model, .argc = 1, },
	{ .name = "VERSION", .handler = cmd_version, .argc = 1, },
	{ .name = "SYSTEM", .handler = cmd_system, .argc = 1, },
	{ .name = "CALIBRATION", .handler = cmd_calibration, .argc = 1, },
	{ .name = "RCALIBRATION", .handler = cmd_rcalibration, .argc = 1, },
	{ .name = "LIMITS", .handler = cmd_limits, .argc = 1, },
	{ .name = "CONFIG", .handler = cmd_config, .argc = 1, },
	{ .name = "STATUS", .handler = cmd_status, .argc = 1, },
	{ .name = "RSTATUS", .handler = cmd_rstatus, .argc = 1, },
	{ .name = "COMMIT", .handler = cmd_commit, .argc = 1, },
	{ .name = "SAVE", .handler = cmd_save, .argc = 1, },
	{ .name = "LOAD", .handler = cmd_load, .argc = 1, },
	{ .name = "RESTORE", .handler = cmd_restore, .argc = 1, },
#if DEBUG
	{ .name = "STUCK", .handler = cmd_stuck, .argc = 1, },
#endif
	{ .name = "SNAME", .handler = cmd_sname, .argc = 2, },
	{ .name = "OUTPUT", .handler = cmd_output, .argc = 2, },
	{ .name = "VOLTAGE", .handler = cmd_voltage, .argc = 2, },
	{ .name = "CURRENT", .handler = cmd_current, .argc = 2, },
	{ .name = "AUTOCOMMIT", .handler = cmd_autocommit, .argc = 2, },
	{ .name = "CALVINADCA", .handler = cmd_cal_vin_adc_a, .argc = 2, },
	{ .name = "CALVINADCB", .handler = cmd_cal_vin_adc_b, .argc = 2, },
	{ .name = "CALVOUTADCA", .handler = cmd_cal_vout_adc_a, .argc = 2, },
	{ .name = "CALVOUTADCB", .handler = cmd_cal_vout_adc_b, .argc = 2, },
	{ .name = "CALVOUTPWMA", .handler = cmd_cal_vout_pwm_a, .argc = 2, },
	{ .name = "CALVOUTPWMB", .handler = cmd_cal_vout_pwm_b, .argc = 2, },
	{ .name = "CALCOUTADCA", .handler = cmd_cal_cout_adc_a, .argc = 2, },
	{ .name = "CALCOUTADCB", .handler = cmd_cal_cout_adc_b, .argc = 2, },
	{ .name = "CALCOUTPWMA", .handler = cmd_cal_cout_pwm_a, .argc = 2, },
	{ .name = "CALCOUTPWMB", .handler = cmd_cal_cout_pwm_b, .argc = 2, },
};

#define MAX_ARGC 2

inline uint8_t split_args(uint8_t **argv, uint8_t size)
{
	uint8_t *word, word_found = 1;
	uint8_t argc = 0;

	// Eliminate the CR/LF character
	uart_read_buf[uart_read_len - 1] = 0;

	for (word = uart_read_buf; *word; word++) {
		if (*word == ' ') {
			*word = 0;
			word_found = 1;
		} else if (word_found) {
			argv[argc++] = word;
			if (argc >= size) {
				argc = 0;
				break;
			}
			word_found = 0;
		}
	}

	argv[argc] = NULL;

	uart_read_len = 0;
	read_newline = 0;

	return argc;
}

inline const struct command *lookup_command(uint8_t *name)
{
	uint8_t idx;

	for (idx = 0; idx < ARRAY_SIZE(commands); idx++) {
		if (strcmp(name, commands[idx].name) == 0) {
			return &commands[idx];
		}
	}

	return NULL;
}

inline void process_input()
{
	uint8_t argc, *argv[MAX_ARGC + 1];
	const struct command *cmd = NULL;

	argc = split_args(argv, ARRAY_SIZE(argv));
	if (argc >= 1) {
		cmd = lookup_command(argv[0]);
	}

	if (!cmd) {
		uart_write_str("UNKNOWN COMMAND\r\n");
	} else if (argc != cmd->argc) {
		uart_write_str("ARGUMENT ERROR\r\n");
	} else {
		cmd->handler(argv[1]);
	}

	uart_write_str("DONE\r\n");
}

inline void clk_init()
{
	CLK_CKDIVR = 0x00; // Set the frequency to 16 MHz
}

inline void pinout_init()
{
	// PA1 is 74HC595 SHCP, output
	// PA2 is 74HC595 STCP, output
	// PA3 is CV/CC leds, output (& input to disable)
	PA_ODR = 0;
	PA_DDR = (1<<1) | (1<<2);
	PA_CR1 = (1<<1) | (1<<2) | (1<<3);
	PA_CR2 = (1<<1) | (1<<2) | (1<<3);

	// PB4 is Enable control, output
	// PB5 is CV/CC sense, input
	PB_ODR = (1<<4); // For safety we start with off-state
	PB_DDR = (1<<4);
	PB_CR1 = (1<<4);
	PB_CR2 = 0;

	// PC3 is unknown, input
	// PC4 is Iout sense, input adc, AIN2
	// PC5 is Vout control, output
	// PC6 is Iout control, output
	// PC7 is Button 1, input
	PC_ODR = 0;
	PC_DDR = (1<<5) | (1<<6);
	PC_CR1 = (1<<7); // For the button
	PC_CR2 = (1<<5) | (1<<6);

	// PD1 is Button 2, input
	// PD2 is Vout sense, input adc, AIN3
	// PD3 is Vin sense, input adc, AIN4
	// PD4 is 74HC595 DS, output
	PD_DDR = (1<<4);
	PD_CR1 = (1<<1) | (1<<4); // For the button
	PD_CR2 = (1<<4);
}

inline void config_load(void)
{
	config_load_system(&cfg_system);
	config_load_output(&cfg_output);

	cfg_system.output = !!cfg_system.default_on;

#if DEBUG
	state.pc3 = 1;
#endif
}

inline void read_state(void)
{
	uint8_t tmp;

#if DEBUG
	tmp = (PC_IDR & (1<<3)) ? 1 : 0;
	if (state.pc3 != tmp) {
		uart_write_str("PC3 is now ");
		uart_write_ch('0' + tmp);
		uart_write_str("\r\n");
		state.pc3 = tmp;
	}
#endif

	tmp = (PB_IDR & (1<<5)) ? 1 : 0;
	if (state.constant_current != tmp) {
		state.constant_current = tmp;
		output_check_state(&cfg_system, state.constant_current);
	}

	if (adc_ready()) {
		uint16_t val = adc_read();
		uint8_t ch = adc_channel();

		switch (ch) {
			case 2:
				state.cout_raw = val;
				// Calculation: val * cal_cout_a * 3.3 / 1024 - cal_cout_b
				state.cout = adc_to_volt(val, &cfg_system.cout_adc);
				ch = 3;
				break;
			case 3:
				state.vout_raw = val;
				// Calculation: val * cal_vout_a * 3.3 / 1024 - cal_vout_b
				state.vout = adc_to_volt(val, &cfg_system.vout_adc);
				ch = 4;
				break;
			case 4:
				state.vin_raw = val;
				// Calculation: val * cal_vin * 3.3 / 1024
				state.vin = adc_to_volt(val, &cfg_system.vin_adc);
				ch = 2;
				{
					uint8_t ch1;
					uint8_t ch2;
					uint8_t ch3;
					uint8_t ch4;

					ch1 = '0' + (state.vin / 10000) % 10;
					ch2 = '0' + (state.vin / 1000) % 10;
					ch3 = '0' + (state.vin / 100) % 10;
					ch4 = '0' + (state.vin / 10 ) % 10;

					display_show(ch1, 0, ch2, 1, ch3, 0, ch4, 0);
				}
				break;
		}

		adc_start(ch);
	}
}

inline void ensure_afr0_set(void)
{
	if ((OPT2 & 1) == 0) {
		uart_flush_writes();
		if (eeprom_set_afr0()) {
			uart_write_str("AFR0 set, reseting the unit\r\n");
			uart_flush_writes();
			iwatchdog_init();
			while (1); // Force a reset in a few msec
		}
		else {
			uart_write_str("AFR0 not set and programming failed!\r\n");
		}
	}
}

int main()
{
	pinout_init();
	clk_init();
	uart_init();
	pwm_init();
	adc_init();

	config_load();

	uart_write_str("\r\n" MODEL " starting: Version " FW_VERSION "\r\n");

	ensure_afr0_set();

	iwatchdog_init();
	adc_start(4);
	commit_output();

	do {
		iwatchdog_tick();
		read_state();
		display_refresh();
		uart_drive();
		if (read_newline) {
			process_input();
		}
	} while(1);
}
