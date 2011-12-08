# Makefile
objs = clist_benchmark.o clist.o

clist_benchmark: Makefile $(objs)
	cc -Wall -o clist_benchmark $(objs) -DDEBUG -lpthread

clist_benchmark.o: clist_benchmark.c clist.h
	cc -Wall -c clist_benchmark.c -DDEBUG

clist.o: clist.c clist.h
	cc -Wall -c clist.c -DDEBUG

clean:
	rm -f *.o *~
