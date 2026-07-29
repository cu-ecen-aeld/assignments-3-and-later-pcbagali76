CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror
TARGET = writer

.PHONY: all clean

all: $(TARGET)

$(TARGET): writer.o
	$(CC) $(CFLAGS) -o $@ $^

writer.o: writer.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) *.o
