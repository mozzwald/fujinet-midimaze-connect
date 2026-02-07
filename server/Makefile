CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=
BUILD_DIR ?= .

TARGET ?= mmsrv
SRC = main.c
OBJ = $(BUILD_DIR)/$(SRC:.c=.o)

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -f $(BUILD_DIR)/$(TARGET) $(OBJ)

.PHONY: all clean
