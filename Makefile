# Simple Makefile

all: main

main: main.c
	gcc -DVALIDATION -o SOoOS main.c

run: clean main
	./SOoOS

debug:
	gcc -DDEBUG -DVALIDATION -o SOoOS main.c 

clean:
	rm -f SOoOS
