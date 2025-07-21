.PHONY: all clean run

all:
	g++ -std=c++20 -I/opt/homebrew/include logger_tests.cpp -o logger_tests

main:
	g++ -std=c++20 -I/opt/homebrew/include main.cpp -o main

clean:
	rm -f logger_tests

run: all
	./logger_tests