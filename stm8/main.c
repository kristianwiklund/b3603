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

typedef struct calibration_write {
	const char *name;
	calibrate_t *cal;
} calibration_write_t;

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

static void write_newline(void)
{
	uart_write_str("\r\n");
}

static void write_str(const char *prefix, const char *val)
{
	uart_write_str(prefix);
	uart_write_str(": ");
	uart_write_str(val);
	write_newline();
}

static void write_onoff(const char *prefix, uint8_t on)
{
	write_str(prefix, on ? "ON" : "OFF");
}

static void write_millis(const char *prefix, uint16_t val)
{
	uart_write_str(prefix);
	uart_write_str(": ");
	uart_write_millis(val);
	write_newline();
}

static void write_uint(const char *prefix, uint32_t val)
{
	uart_write_str(prefix);
	uart_write_str(": ");
	uart_write_uint(val);
	write_newline();
}

static void write_calibration_fixed_point(calibration_write_t *calwrite)
{
	uart_write_str("CALIBRATE ");
	uart_write_str(calwrite->name);
	uart_write_str(": ");
	uart_write_fixed_point(calwrite->cal->a);
	uart_write_ch('/');
	uart_write_fixed_point(calwrite->cal->b);
	write_newline();
}

static void write_calibration_uint(calibration_write_t *calwrite)
{
	uart_write_str("CALIBRATE ");
	uart_write_str(calwrite->name);
	uart_write_str(": ");
	uart_write_uint(calwrite->cal->a);
	uart_write_ch('/');
	uart_write_uint(calwrite->cal->b);
	write_newline();
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
	void (*handler)(const struct command *cmd, uint8_t **argv);
	uint8_t argc;
	void *aux;
} command_t;

static void cmd_sname(const command_t *cmd, uint8_t **argv)
{
	uint8_t *name = argv[1];
	uint8_t idx;

	(void) cmd;

	for (idx = 0; name[idx] != 0; idx++) {
		if (!isprint(name[idx]))
			name[idx] = '.'; // Eliminate non-printable chars
	}

	memcpy(cfg_system.name, name, sizeof(cfg_system.name));
	cfg_system.name[sizeof(cfg_system.name)-1] = 0;

	write_str("SNAME", cfg_system.name);
}

static void cmd_set_bool(const command_t *cmd, uint8_t **argv)
{
	uint8_t *val = cmd->aux;
	uint8_t *s = argv[1];

	if (((s[0] & ~1) == '0') && s[1] == 0) {
		*val = s[0] & 1;
		write_onoff(cmd->name, *val);
		autocommit();
	} else {
		uart_write_str(cmd->name);
		uart_write_str(" takes either 0 for OFF or 1 for ON, received: \"");
		uart_write_str(s);
		uart_write_str("\"\r\n");
	}
}


typedef struct cmd_set_output_value_aux {
	uint16_t *val;
	uint16_t min;
	uint16_t max;
} cmd_set_output_value_aux_t;

static void cmd_set_output_value(const command_t *cmd, uint8_t **argv)
{
	cmd_set_output_value_aux_t *aux = cmd->aux;
	uint8_t *s = argv[1];
	fixed_t val;

	(void) cmd;

	val = parse_millinum(s);
	if (val == 0xFFFF)
		return;

	if (val > aux->max) {
		uart_write_str(cmd->name);
		uart_write_str(" VALUE TOO HIGH\r\n");
		return;
	}
	if (val < aux->min) {
		uart_write_str(cmd->name);
		uart_write_str(" VALUE TOO LOW\r\n");
		return;
	}

	write_millis(cmd->name, val);
	*aux->val = val;

	autocommit();
}

static const cmd_set_output_value_aux_t cmd_set_output_value_aux_voltage = { .val = &cfg_output.vset, .min = CAP_VMIN, .max = CAP_VMAX, };
static const cmd_set_output_value_aux_t cmd_set_output_value_aux_current = { .val = &cfg_output.cset, .min = CAP_CMIN, .max = CAP_CMAX, };

typedef struct cmd_cal_aux {
	const char *name;
	uint32_t *val;
} cmd_cal_aux_t;

static void cmd_cal(const command_t *cmd, uint8_t **argv)
{
	cmd_cal_aux_t *aux = cmd->aux;
	uint8_t *s = argv[1];
	uint8_t *stop;
	uint8_t digits;

	*aux->val = parse_num(s, &stop, &digits);
	uart_write_str("CALIBRATION SET ");
	uart_write_str(aux->name);
	write_newline();
}

static const cmd_cal_aux_t cmd_cal_aux_vin_adc_a = { .name = "VIN ADC A", .val = &cfg_system.vin_adc.a, };
static const cmd_cal_aux_t cmd_cal_aux_vin_adc_b = { .name = "VIN ADC B", .val = &cfg_system.vin_adc.b, };
static const cmd_cal_aux_t cmd_cal_aux_vout_adc_a = { .name = "VOUT ADC A", .val = &cfg_system.vout_adc.a, };
static const cmd_cal_aux_t cmd_cal_aux_vout_adc_b = { .name = "VOUT ADC B", .val = &cfg_system.vout_adc.b, };
static const cmd_cal_aux_t cmd_cal_aux_vout_pwm_a = { .name = "VOUT PWM A", .val = &cfg_system.vout_pwm.a, };
static const cmd_cal_aux_t cmd_cal_aux_vout_pwm_b = { .name = "VOUT PWM B", .val = &cfg_system.vout_pwm.b, };
static const cmd_cal_aux_t cmd_cal_aux_cout_adc_a = { .name = "COUT ADC A", .val = &cfg_system.cout_adc.a, };
static const cmd_cal_aux_t cmd_cal_aux_cout_adc_b = { .name = "COUT ADC B", .val = &cfg_system.cout_adc.b, };
static const cmd_cal_aux_t cmd_cal_aux_cout_pwm_a = { .name = "COUT PWM A", .val = &cfg_system.cout_pwm.a, };
static const cmd_cal_aux_t cmd_cal_aux_cout_pwm_b = { .name = "COUT PWM B", .val = &cfg_system.cout_pwm.b, };

static void cmd_model(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	write_str("MODEL", MODEL);
}

static void cmd_version(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	write_str("VERSION", FW_VERSION);
}

static void cmd_system(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	cmd_model(NULL, NULL);
	cmd_version(NULL, NULL);
	write_str("NAME", cfg_system.name);
	write_onoff("ONSTARTUP", cfg_system.default_on);
	write_onoff("AUTOCOMMIT", cfg_system.autocommit);
}

static const calibration_write_t calibration_write[] = {
	{ .name = "VIN ADC", .cal = &cfg_system.vin_adc, },
	{ .name = "VOUT ADC", .cal = &cfg_system.vout_adc, },
	{ .name = "COUT ADC", .cal = &cfg_system.cout_adc, },
	{ .name = "VOUT PWM", .cal = &cfg_system.vout_pwm, },
	{ .name = "COUT PWM", .cal = &cfg_system.cout_pwm, },
};

typedef void (*calibration_write_func_t)(calibration_write_t *cal);

static void cmd_calibration(const command_t *cmd, uint8_t **argv)
{
	calibration_write_func_t handler = cmd->aux;
	uint8_t idx;

	(void) argv;

	for (idx = 0; idx < ARRAY_SIZE(calibration_write); idx++) {
		handler(&calibration_write[idx]);
	}
}

static void cmd_limits(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	uart_write_str("LIMITS:\r\n");
	write_millis("VMIN", CAP_VMIN);
	write_millis("VMAX", CAP_VMAX);
	write_millis("VSTEP", CAP_VSTEP);
	write_millis("CMIN", CAP_CMIN);
	write_millis("CMAX", CAP_CMAX);
	write_millis("CSTEP", CAP_CSTEP);
}

static void cmd_config(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	uart_write_str("CONFIG:\r\n");
	write_onoff("OUTPUT", cfg_system.output);
	write_millis("VSET", cfg_output.vset);
	write_millis("CSET", cfg_output.cset);
	write_millis("VSHUTDOWN", cfg_output.vshutdown);
	write_millis("CSHUTDOWN", cfg_output.cshutdown);
}

static void cmd_status(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	uart_write_str("STATUS:\r\n");
	write_onoff("OUTPUT", cfg_system.output);
	write_millis("VIN", state.vin);
	write_millis("VOUT", state.vout);
	write_millis("COUT", state.cout);
	write_str("CONSTANT", state.constant_current ? "CURRENT" : "VOLTAGE");
}

static void cmd_rstatus(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	uart_write_str("RSTATUS:\r\n");
	write_onoff("OUTPUT", cfg_system.output);
	write_uint("VIN ADC", state.vin_raw);
	write_millis("VIN", state.vin);
	write_uint("VOUT ADC", state.vout_raw);
	write_millis("VOUT", state.vout);
	write_uint("COUT ADC", state.cout_raw);
	write_millis("COUT", state.cout);
	write_str("CONSTANT", state.constant_current ? "CURRENT" : "VOLTAGE");
}

static void cmd_commit(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	commit_output();
}

static void cmd_save(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	config_save_system(&cfg_system);
	config_save_output(&cfg_output);
	uart_write_str("SAVED\r\n");
}

static void cmd_load(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	config_load_system(&cfg_system);
	config_load_output(&cfg_output);
	autocommit();
}

static void cmd_restore(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	config_default_system(&cfg_system);
	config_default_output(&cfg_output);
	autocommit();
}

#if DEBUG
static void cmd_stuck(const command_t *cmd, uint8_t **argv)
{
	(void) cmd;
	(void) argv;

	// Allows debugging of the IWDG feature
	uart_write_str("STUCK\r\n");
	uart_write_flush();
	while(1); // Induce watchdog reset
}
#endif

static void cmd_help(const command_t *cmd, uint8_t **argv);

static const command_t commands[] = {
	{ .name = "MODEL", .handler = cmd_model, .argc = 1, },
	{ .name = "VERSION", .handler = cmd_version, .argc = 1, },
	{ .name = "SYSTEM", .handler = cmd_system, .argc = 1, },
	{ .name = "CALIBRATION", .handler = cmd_calibration, .argc = 1, .aux = write_calibration_fixed_point, },
	{ .name = "RCALIBRATION", .handler = cmd_calibration, .argc = 1, .aux = write_calibration_uint, },
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
	{ .name = "HELP", .handler = cmd_help, .argc = 1, },
	{ .name = "SNAME", .handler = cmd_sname, .argc = 2, },
	{ .name = "ECHO", .handler = cmd_set_bool, .argc = 2, .aux = &uart_echo, },
	{ .name = "OUTPUT", .handler = cmd_set_bool, .argc = 2, .aux = &cfg_system.output, },
	{ .name = "VOLTAGE", .handler = cmd_set_output_value, .argc = 2, .aux = &cmd_set_output_value_aux_voltage, },
	{ .name = "CURRENT", .handler = cmd_set_output_value, .argc = 2, .aux = &cmd_set_output_value_aux_current, },
	{ .name = "AUTOCOMMIT", .handler = cmd_set_bool, .argc = 2, .aux = &cfg_system.autocommit, },
	{ .name = "ONSTARTUP", .handler = cmd_set_bool, .argc = 2, .aux = &cfg_system.default_on, },
	{ .name = "CALVINADCA", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_vin_adc_a, },
	{ .name = "CALVINADCB", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_vin_adc_b, },
	{ .name = "CALVOUTADCA", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_vout_adc_a, },
	{ .name = "CALVOUTADCB", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_vout_adc_b, },
	{ .name = "CALVOUTPWMA", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_vout_pwm_a, },
	{ .name = "CALVOUTPWMB", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_vout_pwm_b, },
	{ .name = "CALCOUTADCA", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_cout_adc_a, },
	{ .name = "CALCOUTADCB", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_cout_adc_b, },
	{ .name = "CALCOUTPWMA", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_cout_pwm_a, },
	{ .name = "CALCOUTPWMB", .handler = cmd_cal, .argc = 2, .aux = &cmd_cal_aux_cout_pwm_b, },
};

#define MAX_ARGC 2

static void cmd_help(const command_t *cmd, uint8_t **argv)
{
	uint8_t idx;

	(void) cmd;
	(void) argv;

	uart_write_str("Available commands:\r\n");
	for (idx = 0; idx < ARRAY_SIZE(commands); idx++) {
		uart_write_str("- ");
		uart_write_str(commands[idx].name);
		write_newline();
		uart_flush_writes();
		iwatchdog_tick();
	}
}

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
		cmd->handler(cmd, argv);
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
	tmp = (PC_IDR >> 3 ) & 1;
	if (state.pc3 != tmp) {
		uart_write_str("PC3 is now ");
		uart_write_ch('0' + tmp);
		uart_write_str("\r\n");
		state.pc3 = tmp;
	}
#endif

	tmp = (PB_IDR >> 5) & 1;
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
