</$objtype/mkfile

TARG=clone
OFILES=clone.$O
BIN=/bin/$objtype
MAN=/sys/man/1

</sys/src/cmd/mkone

install:V: man
	
uninstall:V:
	rm -f $BIN/$TARG
	rm -f $MAN/$TARG
