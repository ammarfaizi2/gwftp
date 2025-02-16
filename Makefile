# SPDX-License-Identifier: GPL-2.0-only

VERSION = 0
PATCHLEVEL = 0
SUBLEVEL = 1
EXTRAVERSION =

CC ?= gcc
CXX ?= g++

SOURCES	= \
	ev_epoll.c \
	ev_io_uring.c \
	gw_stack.c \
	gwftp.c \
	validator.c

DEFINE_FLAGS = \
	-D_GNU_SOURCE \
	-DGWFTP_VERSION=\"$(VERSION).$(PATCHLEVEL).$(SUBLEVEL)$(EXTRAVERSION)\" \
	-DGWFTP_VERSION_MAJOR=$(VERSION) \
	-DGWFTP_VERSION_MINOR=$(PATCHLEVEL) \
	-DGWFTP_VERSION_PATCH=$(SUBLEVEL) \
	-DGWFTP_VERSION_EXTRA=\"$(EXTRAVERSION)\"

DEPFLAGS	= -MMD -MP -MF $*.d -MT $*.o
CFLAGS		= -Wall -Wextra -O2 -ggdb3 $(DEPFLAGS) $(DEFINE_FLAGS) -std=gnu11
CXXFLAGS	= -Wall -Wextra -O2 -ggdb3 $(DEPFLAGS) $(DEFINE_FLAGS) -std=gnu++17
LDFLAGS		= -O2 -ggdb3
LDLIBS		= -lpthread

all: gwftp

gwftp: $(SOURCES:.c=.o)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

-include $(SOURCES:.c=.d)

clean:
	rm -vf gwftp *.o *.d

.PHONY: all clean
