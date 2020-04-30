#!/bin/bash

echo "Resetting device..."
openocd -f interface/stlink.cfg -f target/stm8s003.cfg -c "init;reset run;shutdown" > /dev/null 2> /dev/null
echo "Turning on output"
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --config "enabled=on" --set 
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --samples 2| tail -1
echo "Setting 3.3 volts output"
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --config "voltage_target=3.3" --set
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --samples 2| tail -1
echo "Turning off output"
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --config "enabled=off" --set
sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --samples 5| tail -1
#sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --show -l 5 2>&1 | tee /tmp/sigrtext
#egrep "Send|Receiv|value" /tmp/sigrtext
