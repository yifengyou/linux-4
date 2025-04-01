
default:
	gcc -c shell.c -o shell.o
	as sys.S -o sys.o
	ld sys.o shell.o -o shell --entry main -z noexecstack
	echo shell | cpio -H newc -o > shell.cpio


