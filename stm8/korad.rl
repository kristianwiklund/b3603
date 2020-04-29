#include "fixedpoint.h"
#include "stm8s.h"
#include "capabilities.h"
#include "uart.h"
#define uws(x) uart_write_str(x"\r")

%%{
machine korad;

action print_idn {uws(MODEL);}
action print_status {uws(MODEL);}

idnq = '*IDN?' @ print_idn;
statusq = 'STATUS?' @ print_status;
vsetq = 'VSET1?';
voutq = 'VOUT1?';
isetq = 'ISET1?';
ioutq = 'IOUT1?';
outon = 'OUT1';
outoff = 'OUT0';
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