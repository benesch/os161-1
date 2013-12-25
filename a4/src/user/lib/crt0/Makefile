#
# Makefile for crt0.o
#

TOP=../../..
.include "$(TOP)/mk/os161.config.mk"

SRCS=$(MACHINE)/crt0.S
MKDIRS=$(INSTALLTOP)/lib $(OSTREE)/lib $(MYBUILDDIR)

.include "$(TOP)/mk/os161.baserules.mk"
.include "$(TOP)/mk/os161.compile.mk"

all-local: $(MYBUILDDIR) .WAIT $(OBJS)

install-staging-local: $(INSTALLTOP)/lib
	cp $(OBJS) $(INSTALLTOP)/lib/

install-local: $(OSTREE)/lib
	cp $(OBJS) $(OSTREE)/lib/

