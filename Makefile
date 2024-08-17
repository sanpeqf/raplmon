build:
	gcc -Wall -Werror -static -o raplmon raplmon.c -lbfdev
clean:
	rm -rf raplmon
run:
	./raplmon
.PHONY: build clean run
