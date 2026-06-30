TARGET  = TurboPDF.tpd
OBJECTS = device.o
SRCDIRS = .
INCDIRS = .

# MorphOS cross-compiler
CC      = ppc-morphos-gcc
LD      = ppc-morphos-gcc

# Toggle DEBUG via:  make debug=1
ifeq ($(debug),1)
 DIR = build-debug
 CFLAGS_EXTRA = -g -O0 -DDEBUG
 LDFLAGS_EXTRA =
else
 DIR = build-release
 CFLAGS_EXTRA = -s -O2
 LDFLAGS_EXTRA = -s
endif

CFLAGS  = -mcpu=750 -Wall -Wextra -Wno-unused-parameter \
          -fomit-frame-pointer -nostdlib -nostartfiles \
          $(addprefix -I,$(INCDIRS)) $(CFLAGS_EXTRA)
LDFLAGS = -mcpu=750 -nostdlib -nostartfiles \
          -lhpdf $(LDFLAGS_EXTRA)

OBJS    = $(addprefix $(DIR)/,$(OBJECTS))

vpath %.c $(SRCDIRS)

all: $(DIR) $(OBJS)
	$(LD) $(CFLAGS) -o $(DIR)/$(TARGET) $(OBJS) $(LDFLAGS)

$(DIR):
	@mkdir -p $(DIR)

$(DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build-release build-debug

.PHONY: all clean
