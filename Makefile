


RELEASE=/Release /D_WIN32 /DNDEBUG /D_CONSOLE
DEBUG=/D_WIN32 /D_DEBUG /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS

CFLAGS = $(DEBUG)

wmenu.exe: wmenu.c arg.h deps/fzy/src/match.c deps/fzy/config.h deps/fzy/src/strings.h
	cl wmenu.c deps/fzy/src/match.c /I deps/fzy/src $(CFLAGS) user32.lib comctl32.lib gdi32.lib

test: test_stream.c wmenu.c arg.h deps/fzy/src/match.c deps/fzy/config.h deps/fzy/src/strings.h
	cl test_stream.c deps/fzy/src/match.c /I deps/fzy/src $(CFLAGS) user32.lib comctl32.lib gdi32.lib /Fe:test_stream.exe
	./test_stream.exe

stress: test
	./test_stream.exe stress

deps/fzy/config.h: deps/fzy/src/config.def.h
	printf '#include "src/config.def.h"\n' > deps/fzy/config.h

deps/fzy/src/strings.h:
	printf '/* generated for MSVC: fzy match.c does not need strings.h here */\n' > deps/fzy/src/strings.h

wmenu-mingw.exe: wmenu.c arg.h deps/fzy/src/match.c deps/fzy/config.h deps/fzy/src/strings.h
	/usr/bin/x86_64-w64-mingw32-gcc -municode -I deps/fzy/src -o wmenu-mingw.exe wmenu.c deps/fzy/src/match.c -luser32 -lcomctl32 -lgdi32
