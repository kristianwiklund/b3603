echo "slå på med knappen på mätaren håll nere 2 sek"
sigrok-cli --driver uni-t-ut61d:conn=1a86.e008 -O analog --continuous


