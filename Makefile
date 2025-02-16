# SPDX-License-Identifier: GPL-2.0-only

CC ?= gcc
CXX ?= g++
DEPFLAGS ?= -MMD -MP -MF $*.d -MT $*.o
CFLAGS ?= -Wall -Wextra -std=c99 -O2 -ggdb3 $(DEPFLAGS) -D_GNU_SOURCE
CXXFLAGS ?= -Wall -Wextra -std=c++11 -O2 -ggdb3 $(DEPFLAGS) -D_GNU_SOURCE
LDFLAGS ?= -O2 -ggdb3
LDLIBS ?= -lpthread
SOURCES = \
	ev_epoll.c \
	ev_io_uring.c \
	gw_stack.c \
	gwftp.c

all: gwftp

gwftp: $(SOURCES:.c=.o)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

-include $(SOURCES:.c=.d)

clean:
	rm -vf gwftp *.o *.d

.PHONY: all clean
