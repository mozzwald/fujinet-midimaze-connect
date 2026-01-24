# Source Files
SOURCES = mmconn.c

# Program Names
CART_PROGRAM = mmconncart
DISK_PROGRAM = mmconndisk

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
LDFLAGS = -t $(CC65_TARGET) -L $(CC65_HOME)/lib

.SUFFIXES:
.PHONY: all clean

# Default Build Target
all: $(CART_PROGRAM).xex $(DISK_PROGRAM).xex

# Compile C source files to object files
mmconn.cart.o: mmconn.c
	$(CC) -c $(CFLAGS) -DCART -o $@ $<

mmconn.disk.o: mmconn.c
	$(CC) -c $(CFLAGS) -DDISK -o $@ $<

# Link Everything into Final XEX Binary
$(CART_PROGRAM).xex: mmconn.cart.o
	$(CC) $(LDFLAGS) -m $(CART_PROGRAM).map -o $@ $^ $(FUJINET_LIB)

$(DISK_PROGRAM).xex: mmconn.disk.o
	$(CC) $(LDFLAGS) -m $(DISK_PROGRAM).map -o $@ $^ $(FUJINET_LIB)

# Clean up build files
clean:
	rm -f mmconn.cart.o mmconn.disk.o $(CART_PROGRAM).xex $(DISK_PROGRAM).xex \
		$(CART_PROGRAM).map $(DISK_PROGRAM).map
