#
# jvmdump - JVMTI based class file dumper
#
# SPDX-License-Identifier: MIT
#

CC       ?= cc
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
LIBDIR   ?= $(PREFIX)/lib/jvmdump
MAN1DIR  ?= $(PREFIX)/share/man/man1

BUILD    := build
BIN      := bin/jvmdump
AGENT    := lib/libjvmdump.so

JAVAC    := $(shell command -v javac 2>/dev/null)
JAVA_HOME ?= $(shell if [ -n "$(JAVAC)" ]; then d=$$(dirname $$(readlink -f "$(JAVAC)")); dirname $$d; fi)

ifeq ($(wildcard $(JAVA_HOME)/include/jvmti.h),)
  JAVA_HOME := $(firstword $(wildcard /usr/lib/jvm/default /usr/lib/jvm/java-*-openjdk /usr/lib/jvm/java-*-jdk /usr/lib/jvm/java-*-temurin))
endif
ifeq ($(wildcard $(JAVA_HOME)/include/jvmti.h),)
  $(error Cannot find jvmti.h. Install a JDK (e.g. 'pacman -S jdk-openjdk') or set JAVA_HOME)
endif

INCLUDES := -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux

CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -fPIC -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CPPFLAGS += -DJVMD_INSTALL_AGENT='"$(LIBDIR)/libjvmdump.so"'
LDLIBS   +=

OBJS     := $(BUILD)/main.o $(BUILD)/attach.o $(BUILD)/util.o
AGENT_OBJS := $(BUILD)/agent.o $(BUILD)/util.o

.PHONY: all clean install uninstall test

all: $(BIN) $(AGENT)

$(BUILD) bin lib:
	mkdir -p $@

$(BIN): $(OBJS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(AGENT): $(AGENT_OBJS) | lib
	$(CC) $(CFLAGS) -shared -Wl,-z,defs -o $@ $(AGENT_OBJS) -pthread

$(BUILD)/main.o: src/main.c src/common.h src/attach.h src/util.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/attach.o: src/attach.c src/attach.h src/util.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/util.o: src/util.c src/util.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/agent.o: src/agent.c src/common.h src/util.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) -fvisibility=hidden -c -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(MAN1DIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/jvmdump
	install -m 0755 $(AGENT) $(DESTDIR)$(LIBDIR)/libjvmdump.so
	install -m 0644 docs/jvmdump.1 $(DESTDIR)$(MAN1DIR)/jvmdump.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jvmdump $(DESTDIR)$(LIBDIR)/libjvmdump.so \
	      $(DESTDIR)$(MAN1DIR)/jvmdump.1
	-rmdir $(DESTDIR)$(LIBDIR) 2>/dev/null

test: all
	./tests/run.sh

clean:
	rm -rf $(BUILD) bin lib
