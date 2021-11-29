all : main my_context

main : main.c coroutine.c
	gcc -g -Wall -o $@ $^
	./main

my_context: my_context_use.cpp
	g++ -g -Wall -o $@ $^
	./my_context

clean :
	rm main
