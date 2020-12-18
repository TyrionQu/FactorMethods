# Makefile for Linux
#

CC=g++-8

LIBPATH = -Wl,-R -Wl,./lib/ -Wl,-eh-frame-hdr
TARGET = gnfs

INCDIR = -I/usr/local/include/ -I/usr/include/jsoncpp
LIB = $(LIBPATH) -lgmp -ljsoncpp -lpthread

CPPFLAGS = -g -std=c++2a
LDFLAGS = 

SOURCES  = .
CPPFILES = $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
OBJFILES = $(CPPFILES:.cpp=.o)

all : $(OBJFILES)
	$(CC) -o $(TARGET) $(LDFLAGS) $(OBJFILES) $(LIB)

%.o : %.cpp
	$(CC) $(CPPFLAGS) $(INCDIR) -c -o $@ $< 

test : 
	cd test
	make

.PHONY : clean
clean:
	-rm $(OBJFILES)

# DO NOT DELETE
