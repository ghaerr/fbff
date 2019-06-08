FFINC = -I/usr/local/include
FFLIB = -L/usr/local/lib -lavutil -lavformat -lavcodec -lavutil -lswscale -lswresample
NANOXINC = -I../microwindows/src/include
NANOXLIB = -L../microwindows/src/lib -lnano-X
DRAW = nxdraw.o
#DRAW = draw.o
CC = cc
CFLAGS = -Wall -O2 $(FFINC) $(NANOXINC) -Wno-deprecated-declarations
LDFLAGS = $(FFLIB) $(NANOXLIB) -lz -lm -lpthread

all: fbff
.c.o:
	$(CC) -c $(CFLAGS) $<
fbff: fbff.o ffs.o $(DRAW)
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o fbff
