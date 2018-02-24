CFLAGS = 
LIBS = -L../inih-r41/
LDFLAGS = -linih
server-load : server-load.c 
	gcc $(CFLAGS) server-load.c -o server-load $(LIBS) $(LDFLAGS)
install:
	cp server-load /usr/local/bin/
clean :
	rm -f server-load

