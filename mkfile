</$objtype/mkfile

TARG=clone
OFILES=clone.$O
BIN=/$objtype/bin
MAN=/sys/man/1

</sys/src/cmd/mkone

README:
	troff -man -N -rL1000i clone.man | ssam 'x/\n\n\n+/c/\n\n/' >README
	
all: README

install:V: man

uninstall:V:
	rm -f $BIN/$TARG
	rm -f $MAN/$TARG

release:V: clean
	tag=`{hg tags|awk 'NR==2{print $1}'}
	tar c . | gzip >/tmp/$TARG.$tag.tgz
