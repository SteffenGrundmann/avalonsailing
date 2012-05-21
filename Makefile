#
# Top level makefile
#

# modules are all directories that have a Makefile
SUBDIRS = $(shell find . -mindepth 2 -name Makefile | xargs -n 1 dirname)
DATE = $(shell date --iso)
PROJECT = avalonsailing
OUTDIR = ..
INSTALL = install

default: all

all clean test test.run:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done

install installconf installinit:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done
	$(MAKE) installscript

installscript: start_avalon.sh
	$(INSTALL) $+  $(DESTDIR)/usr/bin

tarball: clean
	tar zcpf $(OUTDIR)/$(PROJECT)-$(DATE).tar.gz --exclude .svn --exclude "svn*" .

image: clean
	$(MAKE) -C buildroot $@
