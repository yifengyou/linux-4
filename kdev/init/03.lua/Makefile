.PHONY = lua default clean

default: shell lua
	echo init > filelist
	echo lua >> filelist
	cat filelist | cpio -H newc -o > init.cpio


shell:
	gcc -c shell.c -o shell.o
	as sys.S -o sys.o
	ld sys.o shell.o -o shell --entry main -z noexecstack
	ls -alh shell
	mv shell init

lua:
	wget -c https://www.lua.org/ftp/lua-5.4.7.tar.gz
	tar -xvf lua-5.4.7.tar.gz
	sed -i 's/^MYLDFLAG.*/MYLDFLAGS=-static/' lua-5.4.7/src/Makefile
	cd lua-5.4.7/src/ && make -j`nproc`
	cp -a lua-5.4.7/src/lua .
	ls -alh lua
	ldd lua || :

clean:
	rm -f *.o lua init shell *.cpio

