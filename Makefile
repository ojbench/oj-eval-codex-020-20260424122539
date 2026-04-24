.PHONY: all clean
all:
	gcc -O2 -std=c11 -Wall -Wextra -o code main.c buddy.c

clean:
	rm -f code
