CC     = gcc
KERNEL = FreeRTOS-Kernel
PORT   = $(KERNEL)/portable/ThirdParty/GCC/Posix
MEM    = $(KERNEL)/portable/MemMang

DROP_PROB  ?= 0.2
TIMEOUT_MS ?= 200

SRCS = main.c \
       $(KERNEL)/tasks.c \
       $(KERNEL)/queue.c \
       $(KERNEL)/list.c \
       $(KERNEL)/timers.c \
       $(KERNEL)/event_groups.c \
       $(PORT)/port.c \
       $(PORT)/utils/wait_for_event.c \
       $(MEM)/heap_4.c

INCLUDES = -I$(KERNEL)/include -I$(PORT) -I.
CFLAGS   = -Wall -O0 -g $(INCLUDES) -lpthread -lrt \
           -DDROP_PROB=$(DROP_PROB)f -DTIMEOUT_MS=$(TIMEOUT_MS)

demo: $(SRCS)
	$(CC) $(CFLAGS) -o demo $(SRCS)

clean:
	rm -f demo