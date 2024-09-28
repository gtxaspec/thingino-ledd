# Cross compiler prefix
CROSS_COMPILE ?= mipsel-linux-

# Compiler and stripping
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

# Compilation flags
CFLAGS = -Os -ffunction-sections -fdata-sections -flto
LDFLAGS = -Wl,--gc-sections -Wl,-z,norelro -Wl,--as-needed
DEBUGFLAGS = -g0

# Target executable
TARGET = ledd

# Source files
SRC = ledd.c

# Object files
OBJ = $(SRC:.c=.o)

# Default target
all: $(TARGET)

# Linking step
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(DEBUGFLAGS)
	$(STRIP) $(TARGET)  # Strip the binary to reduce size

# Compilation step
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJ) $(TARGET)
