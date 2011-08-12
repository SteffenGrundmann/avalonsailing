#! /bin/sh

set -e

DESC="Fuelcell monitor"
DIR=/usr/bin
DEVICE=/dev/ttyUSB8


case "$1" in
  start)
        echo -n "Starting $DESC: "
	$DIR/fcmon -p 60 $DEVICE | $DIR/plug -i /var/run/lbus >/dev/null 2>&1 &
        echo "OK"
        ;;
  stop)
        echo -n "Stopping $DESC: "
	kill `cat /var/run/fcmon.pid`
	killall fcmon
        echo "OK"
        ;;
  restart|force-reload)
        echo "Restarting $DESC: "
        $0 stop
        sleep 1
        $0 start
        echo ""
        ;;
  *)
        echo "Usage: $0 {start|stop|restart|force-reload}" >&2
        exit 1
        ;;
esac

exit 0
