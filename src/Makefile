prefix = /usr

CFLAGS := -g -Wall -O2

CFLAGS += $(shell pkg-config --cflags gtk+-2.0 pygtk-2.0)
LDLIBS += $(shell pkg-config --libs gtk+-2.0 pygtk-2.0)

CFLAGS += -pg
LDFLAGS += -pg

CODEGENDIR = $(shell pkg-config --variable=codegendir pygtk-2.0)
DEFSDIR = $(shell pkg-config --variable=defsdir pygtk-2.0)

PYVERSION = $(shell python -c "import sys; print sys.version[:3]")
PYLIBDIR = $(prefix)/lib/python$(PYVERSION)/site-packages

CFLAGS += -I$(prefix)/include/python$(PYVERSION)

all: gen-vnc.defs.c

vnc-test: vnc-test.o gvnc.o coroutine.o continuation.o vncdisplay.o d3des.o

vnc.defs: vncdisplay.h
	python $(CODEGENDIR)/h2def.py $< > $@

gen-vnc.defs.c: vnc.override vnc.defs
	pygtk-codegen-2.0 --prefix vnc --register $(DEFSDIR)/gdk-types.defs \
			  --register $(DEFSDIR)/gtk-types.defs \
			  --override vnc.override vnc.defs > $@

vnc.so: gvnc.o coroutine.o continuation.o vncdisplay.o vncmodule.o \
        gen-vnc.defs.o d3des.o
	$(CC) $(LDFLAGS) $(LDLIBS) -shared $^ -o $@

coroutine.o: coroutine.c coroutine.h continuation.h
gvnc.o: gvnc.c gvnc.h coroutine.h blt.h blt1.h continuation.h d3des.h
continuation.o: continuation.c continuation.h
vncdisplay.o: vncdisplay.c vncdisplay.h
d3des.o: d3des.c d3des.h

clean:
	$(RM) *.o *~ gen-vnc.defs.c vnc.so vnc.defs
