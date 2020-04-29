#!/bin/sh

sigrok-cli -d korad-kaxxxxp:conn=/dev/ttyUSB0:serialcomm=38400/8n1 --show -l 5
