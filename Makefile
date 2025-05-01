# Simple Makefile

all: main

main: main.c
	gcc -o SOoOS main.c

run: clean main
	./SOoOS
clean:
	rm -f SOoOS
