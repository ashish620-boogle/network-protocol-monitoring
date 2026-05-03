CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
LDFLAGS :=
UNAME_S :=
EXEEXT :=

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
LDFLAGS += -lWs2_32
else
UNAME_S := $(shell uname -s 2>/dev/null)
endif
ifneq ($(findstring MINGW,$(UNAME_S)),)
LDFLAGS += -lWs2_32
endif

TARGETS := server$(EXEEXT) client$(EXEEXT)

all: $(TARGETS)

server$(EXEEXT): server.cpp common.h
	$(CXX) $(CXXFLAGS) server.cpp -o server$(EXEEXT) $(LDFLAGS)

client$(EXEEXT): client.cpp common.h
	$(CXX) $(CXXFLAGS) client.cpp -o client$(EXEEXT) $(LDFLAGS)

clean:
ifeq ($(OS),Windows_NT)
	del /Q server.exe client.exe 2>NUL
else
	rm -f $(TARGETS) server.exe client.exe
endif

.PHONY: all clean
