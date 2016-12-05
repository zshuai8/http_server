CFLAGS=-Wall -Werror -Wmissing-prototypes -g -fPIC
LDLIBS=-lpthread
HEADERS=list.h rio.h threadpool.h thread_lib.h

all:		sysstatd

sysstatd:	list.o threadpool.o rio.o

clean:
	rm -f *.o *~ sysstatd
