CC = gcc

CFLAGS = -g -std=c11 -MD -MP  -Wall -Wfatal-errors

OBJGROUP = si1132.o bme280-i2c.o bme280.o weather_board.o dlog.o dpid.o dfork.o dexec.o dsignal.o dzip.o dmem.o dnonblock.o version.o

EXTRA_LIBS = -lwiringPi -lpthread -lcrypt -lrt -lzip

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


#######################
G_EX = $(shell git describe --tag > /dev/null ; if [ $$? -eq 0 ]; then echo "OK"; else echo "FAIL" ; fi)
GVER = $(shell git describe --abbrev=7 --long)
#######################

version.c: FORCE
	@echo "==============================================="
	@echo "git present:" $(G_EX) " ver:" $(GVER)
	@echo "==============================================="
ifeq "$(G_EX)" "OK"
	git describe --tag | awk 'BEGIN { FS="-" } {print "#include \"version.h\""} {print "const char * git_version = \"" $$1"."$$2"\";"} END {}' > version.c
	git rev-parse --abbrev-ref HEAD | awk '{print "const char * git_branch = \""$$0"\";"} {}' >> version.c
endif

FORCE:
