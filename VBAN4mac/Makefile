CC = gcc
CFLAGS = -Wall -Wextra -I./include -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
LDFLAGS = -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
EXAMPLES_DIR = examples

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
EXAMPLES = $(wildcard $(EXAMPLES_DIR)/*.c)
EXAMPLE_BINS = $(EXAMPLES:$(EXAMPLES_DIR)/%.c=$(BUILD_DIR)/%)

.PHONY: all clean examples

all: $(BUILD_DIR)/libvban4mac.a examples

$(BUILD_DIR)/libvban4mac.a: $(OBJS)
	@mkdir -p $(BUILD_DIR)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/%: $(EXAMPLES_DIR)/%.c $(BUILD_DIR)/libvban4mac.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -L$(BUILD_DIR) -lvban4mac $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR) 