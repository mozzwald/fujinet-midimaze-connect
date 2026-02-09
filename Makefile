# Directories
CLIENT_DIR = client
SERVER_DIR = server
BUILD_DIR = build

# Client Source Files
SOURCES = $(CLIENT_DIR)/mmconn.c

# Client Program Names
CART_PROGRAM = mmconncart
DISK_PROGRAM = mmconndisk
SERVER_PROGRAM = mmsrv

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
.PHONY: all clean client server

# Default Build Target
all: client server

client: $(BUILD_DIR)/$(CART_PROGRAM).xex $(BUILD_DIR)/$(DISK_PROGRAM).xex

server:
	$(MAKE) -C $(SERVER_DIR) BUILD_DIR=../$(BUILD_DIR) TARGET=$(SERVER_PROGRAM)

# Compile C source files to object files
$(BUILD_DIR)/mmconn.cart.o: $(CLIENT_DIR)/mmconn.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DCART -o $@ $<

$(BUILD_DIR)/mmconn.disk.o: $(CLIENT_DIR)/mmconn.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DDISK -o $@ $<

# Link Everything into Final XEX Binary
$(BUILD_DIR)/$(CART_PROGRAM).xex: $(BUILD_DIR)/mmconn.cart.o | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -m $(BUILD_DIR)/$(CART_PROGRAM).map -o $@ $^ $(FUJINET_LIB)

$(BUILD_DIR)/$(DISK_PROGRAM).xex: $(BUILD_DIR)/mmconn.disk.o | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -m $(BUILD_DIR)/$(DISK_PROGRAM).map -o $@ $^ $(FUJINET_LIB)
	python3 tools/fix_runad.py $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Clean up build files
clean:
	rm -f $(BUILD_DIR)/mmconn.cart.o $(BUILD_DIR)/mmconn.disk.o \
		$(BUILD_DIR)/$(CART_PROGRAM).xex $(BUILD_DIR)/$(DISK_PROGRAM).xex \
		$(BUILD_DIR)/$(CART_PROGRAM).map $(BUILD_DIR)/$(DISK_PROGRAM).map \
		$(BUILD_DIR)/$(SERVER_PROGRAM)
	$(MAKE) -C $(SERVER_DIR) clean
