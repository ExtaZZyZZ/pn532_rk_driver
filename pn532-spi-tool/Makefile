CC     := aarch64-linux-gnu-gcc
CFLAGS := -Wall -Wextra -O2 -Wno-unused-result
TARGET := pn532-spi-tool
SRCS   := main.c pn532_spi.c pn532_proto.c aes128.c users.c
DAEMON := pn532d
DAEMON_SRC := pn532d.c pn532_spi.c pn532_proto.c aes128.c users.c
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^
$(DAEMON): $(DAEMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^
clean:
	rm -f $(TARGET) $(DAEMON)
deploy: $(TARGET)
	scp $(TARGET) root@$(BOARD_IP):/usr/local/bin/

.PHONY: all clean deploy
