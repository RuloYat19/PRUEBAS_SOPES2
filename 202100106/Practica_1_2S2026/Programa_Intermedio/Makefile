CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = prueba
SRC = Prueba.C

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
