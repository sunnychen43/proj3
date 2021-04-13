CC = gcc
CFLAGS = -g -c -m32
AR = ar -rc
RANLIB = ranlib

all: my_vm.a

my_vm.a: my_vm.o bit.o
	$(AR) libmy_vm.a my_vm.o bit.o
	$(RANLIB) libmy_vm.a

my_vm.o: my_vm.h
	$(CC)	$(CFLAGS)  my_vm.c -lpthread

bit.o: bit.h
	$(CC)	$(CFLAGS)  bit.c

clean:
	rm -rf *.o *.a
