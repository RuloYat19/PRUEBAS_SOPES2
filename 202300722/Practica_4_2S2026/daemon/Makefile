CC = gcc

CFLAGS = -Wall -Wextra -O2 -pthread

TARGET = daemon

all:
	$(CC) $(CFLAGS) daemon.c -o $(TARGET)

clean:
	rm -f $(TARGET)