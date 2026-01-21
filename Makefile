# Source Files
SOURCES = mmconn.c

# Program Name
PROGRAM = mmconn

export CC65_HOME = /usr/share/cc65

# cc65 Target System
CC65_TARGET = atari

# FujiNet Library
FUJINET_LIB_VERSION = 4.9.0
FUJINET_LIB_DIR = fujinet-lib-$(CC65_TARGET)-$(FUJINET_LIB_VERSION)
FUJINET_LIB = $(FUJINET_LIB_DIR)/fujinet-$(CC65_TARGET)-$(FUJINET_LIB_VERSION).lib
FUJINET_INCLUDES = -I$(FUJINET_LIB_DIR)

# Compiler & Linker Settings
CC = cl65
CFLAGS = -t $(CC65_TARGET) -O $(FUJINET_INCLUDES)
LDFLAGS = -t $(CC65_TARGET) -m $(PROGRAM).map -L $(CC65_HOME)/lib

.SUFFIXES:
.PHONY: all clean

# Default Build Target
all: $(PROGRAM).xex

# Compile C source files to object files
%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

# Link Everything into Final XEX Binary
$(PROGRAM).xex: $(SOURCES:.c=.o)
	$(CC) $(LDFLAGS) -o $@ $^ $(FUJINET_LIB)

# Clean up build files
clean:
	rm -f $(SOURCES:.c=.o) $(PROGRAM).xex src/$(PROGRAM).map