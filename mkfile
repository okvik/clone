</$objtype/mkfile

TARG=clone
OFILES=clone.$O
BIN=/$objtype/bin

</sys/src/cmd/mkone

README:
	troff -man -N -rL1000i clone.man | ssam 'x/\n\n\n+/c/\n\n/' >README
	
all: README

manpage:V:
	cp clone.man /sys/man/1/clone

install:V: manpage

uninstall:V:
	rm -f $BIN/$TARG
	rm -f /sys/man/1/clone

release:V: clean
	tag=`{hg tags|awk 'NR==2{print $1}'}
	tar c . | gzip >/tmp/$TARG.$tag.tgz
