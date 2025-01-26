CC = gcc
CFLAGS = -Wall -Wextra -I./include -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
LDFLAGS = -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
EXAMPLES_DIR = examples

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

EXAMPLES = simple_bridge device_bridge monitor_bridge
EXAMPLE_BINS = $(EXAMPLES:%=$(BUILD_DIR)/%)

.PHONY: all clean

all: $(BUILD_DIR)/libvban4mac.a $(EXAMPLE_BINS)

$(BUILD_DIR)/libvban4mac.a: $(OBJS)
	@mkdir -p $(BUILD_DIR)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%: $(EXAMPLES_DIR)/%.c $(BUILD_DIR)/libvban4mac.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -L$(BUILD_DIR) -lvban4mac $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR) 