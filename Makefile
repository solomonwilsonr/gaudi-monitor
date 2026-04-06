CC      ?= cc
VERSION  = $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
CFLAGS   = -O3 -march=native -flto -Wall -Wextra -std=gnu11 -DVERSION='"$(VERSION)"'
CFLAGS_PORTABLE = -O3 -flto -Wall -Wextra -std=gnu11 -DVERSION='"$(VERSION)"'
LDFLAGS  = -lncursesw -ldl -lpthread
PREFIX   ?= /usr/local
TARGET   = gaudi-monitor

all: $(TARGET)

$(TARGET): gaudi-monitor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

demo-load: demo-load.c
	$(CC) -O2 -Wall -Wextra -o demo-load demo-load.c -lpthread -lm

portable:
	$(CC) $(CFLAGS_PORTABLE) -o $(TARGET) gaudi-monitor.c $(LDFLAGS)

test: test_meminfo.c
	$(CC) -O0 -Wall -Wextra -o test_meminfo test_meminfo.c
	./test_meminfo

clean:
	rm -f $(TARGET) demo-load test_meminfo

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/

install-user: $(TARGET)
	install -d $(HOME)/.local/bin
	install -m 755 $(TARGET) $(HOME)/.local/bin/

.PHONY: all portable test clean install install-user
