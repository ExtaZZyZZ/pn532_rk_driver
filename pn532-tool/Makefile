CC      := aarch64-linux-gnu-gcc
CFLAGS  := -Wall -Wextra -O2 -Wno-unused-result
TARGET  := pn532-tool
SRCS    := main.c pn532.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)
