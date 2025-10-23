CC = gcc
CFLAGS = -std=c99 -Wall -O2 -static
TARGET = ned

all: $(TARGET)

$(TARGET): main.c config.h
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET) *.o
