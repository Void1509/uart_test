PROJ := $(notdir $(CURDIR))

LIBRARY := glib-2.0 gio-2.0

CFLAGS += `pkg-config --cflags $(LIBRARY)`
CFLAGS += -O2 -std=gnu11

LIBS += `pkg-config --libs $(LIBRARY)`

OBJS = $(patsubst %.c, %.o, $(wildcard *.c))

.PHONY: clean cpflags

all:	$(PROJ)

$(PROJ):	$(OBJS) $(COBJS) $(GOBJS)
	$(CXX) -o $@ $^ $(LIBS)

%.o:	%.c
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

clean:
	rm -fr $(PROJ) $(OBJS) $(COBJS) $(GOBJS)

cpflags:
	echo $(CFLAGS) | sed 's/\s\+/\n/g' > compile_flags.txt

