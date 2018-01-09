# Constants
OS = $(shell uname -s)
ifeq ($(shell printf '\1' | od -dAn | xargs),1)
	BIG_ENDIAN = 0
else
	BIG_ENDIAN = 1
endif

# Variables with default values
CXX?=g++
EXEC?=MonaSRT

# Variables extendable
CFLAGS+=-D_GLIBCXX_USE_C99 -std=c++11 -Wall -Wno-reorder -Wno-terminate -Wunknown-pragmas -Wno-unknown-warning-option -D__BIG_ENDIAN__=$(BIG_ENDIAN) -D_FILE_OFFSET_BITS=64
override INCLUDES+=-I../MonaBase/include/ -I../MonaCore/include/ -I./include -I../ -I./srt/include/
LIBDIRS+=-L../MonaBase/lib/ -L../MonaCore/lib/ -L./srt/lib/
LDFLAGS+="-Wl,-rpath,../MonaBase/lib/,-rpath,../MonaCore/lib/,-rpath,/usr/local/lib/,-rpath,./srt/lib/"
LIBS+=-pthread -lMonaBase -lMonaCore -lcrypto -lssl -lsrt
ifneq ($(OS),FreeBSD)
	LIBS+= -ldl
endif
ifeq ($(OS),Darwin)
	LBITS := $(shell getconf LONG_BIT)
	ifeq ($(LBITS),64)
	   # just require for OSX 64 buts
		LIBS +=  -pagezero_size 10000 -image_base 100000000
	endif
endif

# Variables fixed
SOURCES = $(wildcard $(SRCDIR)sources/*.cpp)
OBJECT = $(SOURCES:sources/%.cpp=tmp/release/%.o)
OBJECTD = $(SOURCES:sources/%.cpp=tmp/debug/%.o)

# pre-build => versionning
$(shell if [ -d "../.git/hooks" ] && [ ! -h "../.git/hooks/pre-commit" ]; then ln -s "../git.hooks.pre-commit" "../.git/hooks/pre-commit"; fi;)

# This line is used to ignore possibly existing folders release/debug
.PHONY: release debug

release:
	mkdir -p tmp/release/
	@$(MAKE) -k $(OBJECT)
	@echo creating executable $(EXEC)
	@$(CXX) $(CFLAGS) -O2 $(LDFLAGS) $(LIBDIRS) -o $(EXEC) $(OBJECT) $(LIBS)

debug:	
	mkdir -p tmp/debug/
	@$(MAKE) -k $(OBJECTD)
	@echo creating debug executable $(EXEC)
	@$(CXX) -g -D_DEBUG $(CFLAGS) -Og $(LDFLAGS) $(LIBDIRS) -o $(EXEC) $(OBJECTD) $(LIBS)

$(OBJECT): tmp/release/%.o: sources/%.cpp
	@echo compiling $(@:tmp/release/%.o=sources/%.cpp)
	@$(CXX) $(CFLAGS) $(INCLUDES) -c -o $(@) $(@:tmp/release/%.o=sources/%.cpp)

$(OBJECTD): tmp/debug/%.o: sources/%.cpp
	@echo compiling $(@:tmp/debug/%.o=sources/%.cpp)
	@$(CXX) -g -D_DEBUG $(CFLAGS) $(INCLUDES) -c -o $(@) $(@:tmp/debug/%.o=sources/%.cpp)

clean:
	@echo cleaning project $(EXEC)
	@rm -f $(OBJECT) $(EXEC)
	@rm -f $(OBJECTD) $(EXEC)
