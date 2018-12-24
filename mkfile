</$objtype/mkfile

TARG=clone
OFILES=clone.$O
BIN=/$objtype/bin
MAN=/sys/man/1

</sys/src/cmd/mkone

install:V: $TARG.man
	
uninstall:V:
	rm -f $BIN/$TARG
	rm -f $MAN/$TARG
