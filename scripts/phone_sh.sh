#!/bin/bash
# Runs on the phone, over telnet on the USB network, the commands read from stdin and prints the output.
# Usage: phone_sh.sh [wait_seconds] < commands.sh   (phone stopped in the debug shell)
# The phone is at 172.16.42.1, or at $PHONE_HOST if set.
WAIT="${1:-4}"
HOST="${PHONE_HOST:-172.16.42.1}"
{
    sleep 1
    echo 'echo; echo __BEGIN__'
    cat
    echo 'echo __END__'
    sleep "$WAIT"
} | nc -w $((WAIT + 5)) "$HOST" 23 | tr -d '\r' | sed -n '/^__BEGIN__$/,/^__END__$/p' | sed '1d;$d'
