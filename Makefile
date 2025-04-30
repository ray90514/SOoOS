# Simple Makefile

all: main

main: main.c
	gcc -o simulator main.c

run: clean main
	./simulator
clean:
	rm -f main
