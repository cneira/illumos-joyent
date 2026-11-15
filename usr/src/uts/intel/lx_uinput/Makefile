#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License (CDDL), version 1.0.
#
# Copyright 2026 Carlos Neira.
#

UTSBASE = ../..
MODULE = lx_uinput
OBJECTS = $(LX_UINPUT_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE = $(USR_DRV_DIR)/$(MODULE)
CONF_SRCDIR = $(UTSBASE)/common/brand/lx/io

include $(UTSBASE)/intel/Makefile.intel

ALL_TARGET = $(BINARY) $(SRC_CONFILE)
INSTALL_TARGET = $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)

.KEEP_STATE:

def: $(DEF_DEPS)
all: $(ALL_DEPS)
clean: $(CLEAN_DEPS)
clobber: $(CLOBBER_DEPS)
install: $(INSTALL_DEPS)

include $(UTSBASE)/intel/Makefile.targ

$(OBJS_DIR)/%.o: $(UTSBASE)/common/brand/lx/io/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)
