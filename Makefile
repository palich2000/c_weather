CC = gcc

CFLAGS = -g -std=c11 -MD -MP  -Wall -Wfatal-errors

OBJGROUP = si1132.o bme280-i2c.o bme280.o weather_board.o

EXTRA_LIBS = -lwiringPi -lpthread -lcrypt -lrt -lmosquitto

all: weather_board

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $<  -o $@

weather_board: $(OBJGROUP)
	$(CC) -o weather_board  $(OBJGROUP) $(EXTRA_LIBS) -lm

DEPS = $(SRCS:%.c=%.d)


-include $(DEPS)

clean:
	rm -f *.o *.d weather_board

install: weather_board
	install -D -o root -g root ./weather_board /usr/local/bin
