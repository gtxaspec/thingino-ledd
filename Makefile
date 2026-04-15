# Cross compiler prefix
CROSS_COMPILE ?= mipsel-linux-

# Compiler and stripping
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

# Compilation flags
CFLAGS = -Os -ffunction-sections -fdata-sections -flto -pthread
LDFLAGS = -Wl,--gc-sections -Wl,-z,norelro -Wl,--as-needed -pthread
DEBUGFLAGS = -g0

# Directories
SRC_DIR = src
OBJ_DIR = src

# Target executable
TARGET = ledd

# Source files
SRC = $(SRC_DIR)/ledd.c

# Object files
OBJ = $(OBJ_DIR)/ledd.o

# Default target
all: $(TARGET)

# Linking step
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(DEBUGFLAGS)
	$(STRIP) $(TARGET)

# Compilation step
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJ) $(TARGET)

# Format code with clang-format
format:
	clang-format -i $(SRC_DIR)/*.c

.PHONY: all clean format
