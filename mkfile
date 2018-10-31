</$objtype/mkfile

TARG=clone
OFILES=clone.$O
BIN=/$objtype/bin

</sys/src/cmd/mkone

install:V:
	cp clone.man1 /sys/man/1/clone

uninstall:V:
	rm -f $BIN/$TARG
	rm -f /sys/man/1/clone
