#!/bin/sh

openocd -f interface/stlink.cfg -f target/stm8s003.cfg -c "init;reset run;shutdown"
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --config "enabled=on" --set 
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --show -l 5 2>&1 | tee /tmp/sigrtext
egrep "Sending|Received" /tmp/sigrtext
