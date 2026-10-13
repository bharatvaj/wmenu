


RELEASE=/Release /D_WIN32 /DNDEBUG /D_CONSOLE
DEBUG=/D_WIN32 /D_DEBUG /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS

CFLAGS = $(DEBUG)

wmenu.exe: wmenu.c arg.h
	cl wmenu.c $(CFLAGS) user32.lib comctl32.lib gdi32.lib

test: test_stream.c wmenu.c arg.h
	cl test_stream.c $(CFLAGS) user32.lib comctl32.lib gdi32.lib /Fe:test_stream.exe
	./test_stream.exe

stress: test
	./test_stream.exe stress

wmenu-mingw.exe: wmenu.c arg.h
	/usr/bin/x86_64-w64-mingw32-gcc -municode -o wmenu-mingw.exe wmenu.c -luser32 -lcomctl32 -lgdi32
