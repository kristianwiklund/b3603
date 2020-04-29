#include "fixedpoint.h"
#include "stm8s.h"
#include "capabilities.h"
#include "uart.h"
#include "config.h"
#include "outputs.h"
extern cfg_system_t cfg_system;
extern cfg_output_t cfg_output;
extern state_t state;

#define uws(x) uart_write_str(x)

%%{
machine korad;

action print_idn {uws(MODEL);}
action print_status {uart_write_ch(0);} 
action print_vset1 {uart_write_millivolt(cfg_output.vset);}
action print_vout1 {uart_write_millivolt(state.vout);}
action print_iset1 {uart_write_millivolt(cfg_output.cset);}
action print_iout1 {uart_write_millivolt(state.cout);}
action outon {cfg_system.output = 1;		commit_output();}
action outoff {cfg_system.output = 0;		commit_output();}

idnq = '*IDN?' @ print_idn;
statusq = 'STATUS?' @ print_status;
vsetq = 'VSET1?' @ print_vset1;
voutq = 'VOUT1?' @ print_vout1;
isetq = 'ISET1?' @ print_iset1;
ioutq = 'IOUT1?'@ print_iout1;
outon = 'OUT1' @ outon;
outoff = 'OUT0' @ outoff;
ovpon = 'OVP1';
ovpoff = 'OVP0';
ocpon = 'OCP1';
ocpoff = 'OCP0';
track = 'TRACK0';
rcl = 'RCL1';
sav = 'SAV1';
chomp = alnum;
main := (idnq|statusq|vsetq|voutq|isetq|ioutq|outon|outoff|ovpon|ocpon|ocpoff|track|rcl|sav|chomp)*;

}%%

%% write data;

#define BUFSIZE 30

	     int cs, act;	 
     char *ts, *te;
     int stack[1], top;

      static char inbuf[BUFSIZE];


void initmachine () {

	%% write init;

}

void parseinput(uint8_t c) {

char cv = c;
char *p = &cv;
char *pe = p + 1;
//uart_write_ch(c);

	%% write exec;
	}