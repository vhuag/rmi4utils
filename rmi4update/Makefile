CXX ?= g++
CPPFLAGS += -I../include -I./include -I../rmidevice
CPPFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
CXXFLAGS += -Wall
LDFLAGS += -L.
CXXFLAGS += -g -O0 -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
LIBS =  -lrmidevice -lrt
LIBDIR = ../rmidevice
LIBNAME = librmidevice.a
RMI4UPDATESRC = main.cpp firmware_image.cpp rmi4update.cpp updateutil.cpp
RMI4UPDATEOBJ = $(RMI4UPDATESRC:.cpp=.o)
PROGNAME = rmi4update
STATIC_BUILD ?= n
ifeq ($(STATIC_BUILD),y)
LDFLAGS += -static
endif

all: $(PROGNAME)

$(PROGNAME): $(RMI4UPDATEOBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(RMI4UPDATEOBJ) -L$(LIBDIR) $(LIBS) -o $(PROGNAME)

clean:
	rm -f $(RMI4UPDATEOBJ) $(PROGNAME)
