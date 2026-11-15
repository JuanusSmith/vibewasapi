# Cross-compile with MinGW-w64 (Linux/MSYS2).  `make` builds 32-bit (Winamp / WACUP x86),
# `make x64` builds 64-bit (WACUP x64 builds).
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  = -shared -static -static-libgcc -static-libstdc++ -s
LIBS     = -lole32 -luser32

all: x86
x86: build/x86/out_vibewasapi.dll
x64: build/x64/out_vibewasapi.dll

build/x86/out_vibewasapi.dll: out_vibewasapi.cpp out.h resource.h out_vibewasapi.rc
	mkdir -p build/x86
	i686-w64-mingw32-windres out_vibewasapi.rc -O coff -o build/x86/res.o
	i686-w64-mingw32-g++ $(CXXFLAGS) out_vibewasapi.cpp build/x86/res.o $(LDFLAGS) $(LIBS) -o $@

build/x64/out_vibewasapi.dll: out_vibewasapi.cpp out.h resource.h out_vibewasapi.rc
	mkdir -p build/x64
	x86_64-w64-mingw32-windres out_vibewasapi.rc -O coff -o build/x64/res.o
	x86_64-w64-mingw32-g++ $(CXXFLAGS) out_vibewasapi.cpp build/x64/res.o $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf build
