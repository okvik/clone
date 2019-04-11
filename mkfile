</$objtype/mkfile

TARG=clone
OFILES=clone.$O
BIN=$home/bin/$objtype
MAN=/sys/man/1

</sys/src/cmd/mkone

install:V: $TARG.man
	
uninstall:V:
	rm -f $BIN/$TARG
	rm -f $MAN/$TARG
