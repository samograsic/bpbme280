# Makefile for bpbme280
# BME280 sensor telemetry over DTN (ION BP)

# Compiler and flags
CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
LIBS = -L/usr/local/lib -lbp -lici -lm -lpthread

# ION headers
ION_INCDIR = ../ione-code
INCLUDES = -I$(ION_INCDIR)/bpv7/include -I$(ION_INCDIR)/ici/include

# Target and source files
TARGET = bpbme280
SOURCES = bpbme280.c
OBJECTS = bpbme280.o

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LIBS)

# Compile source files
bpbme280.o: bpbme280.c
	$(CC) $(CFLAGS) $(INCLUDES) -c bpbme280.c

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Install system-wide
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall