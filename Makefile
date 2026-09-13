PIDGIN_TREE_TOP ?= ../pidgin-2.10.11
PIDGIN3_TREE_TOP ?= ../pidgin-main
LIBPURPLE_DIR ?= $(PIDGIN_TREE_TOP)/libpurple
WIN32_DEV_TOP ?= $(PIDGIN_TREE_TOP)/../win32-dev

WIN32_CC ?= $(WIN32_DEV_TOP)/mingw-4.7.2/bin/gcc

PKG_CONFIG ?= pkg-config
DIR_PERM = 0755
LIB_PERM = 0755
FILE_PERM = 0644

CFLAGS ?= -O2 -g -ggdb -pipe -std=gnu99 -DGDK_PIXBUF_ENABLE_BACKEND=1 -DPURPLE_PLUGINS
LDFLAGS ?=

ifeq ($(OS),Windows_NT)
  PROGFILES32 = ${ProgramFiles(x86)}
  ifndef PROGFILES32
    PROGFILES32 = $(PROGRAMFILES)
  endif
  PLUGIN_TARGET = gif_loop_limit.dll
  PLUGIN_DEST = "$(PROGFILES32)/Pidgin/plugins"
else
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S), Darwin)
    INCLUDES = -I/opt/local/include $(OS)
    CC ?= gcc
  else
    INCLUDES =
    CC ?= gcc
  endif

  ifeq ($(shell $(PKG_CONFIG) --exists pidgin 2>/dev/null && echo "true"),)
    ifeq ($(shell $(PKG_CONFIG) --exists purple 2>/dev/null && echo "true"),)
      PLUGIN_TARGET = FAILNOPIDGIN
      PLUGIN_DEST =
    else
      PLUGIN_TARGET = gif_loop_limit.so
      PLUGIN_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir purple`
    endif
  else
    PLUGIN_TARGET = gif_loop_limit.so
    PLUGIN_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir pidgin`
  endif
endif

WIN32_GLIB_TOP ?= $(WIN32_DEV_TOP)/glib-2.28.8
WIN32_GTK_TOP ?= $(WIN32_DEV_TOP)/gtk_2_0-2.14

WIN32_CFLAGS = \
	-O0 -g -ggdb \
	-std=gnu99 \
	-DGDK_PIXBUF_ENABLE_BACKEND=1 \
	-DPURPLE_PLUGINS \
	-I$(WIN32_GLIB_TOP)/include \
	-I$(WIN32_GLIB_TOP)/include/glib-2.0 \
	-I$(WIN32_GLIB_TOP)/lib/glib-2.0/include \
	-I$(WIN32_GTK_TOP)/include \
	-I$(WIN32_GTK_TOP)/include/gtk-2.0 \
	-I$(WIN32_GTK_TOP)/lib/gtk-2.0/include \
	-I$(WIN32_GTK_TOP)/include/cairo \
	-I$(WIN32_GTK_TOP)/include/pango-1.0 \
	-I$(WIN32_GTK_TOP)/include/atk-1.0 \
	-I$(PIDGIN_TREE_TOP) \
	-I$(PIDGIN_TREE_TOP)/libpurple \
	-I$(PIDGIN_TREE_TOP)/libpurple/win32 \
	-I$(PIDGIN_TREE_TOP)/pidgin \
	-I$(PIDGIN_TREE_TOP)/pidgin/win32 \
	-DENABLE_NLS \
	-Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing

WIN32_LDFLAGS = \
	-L$(WIN32_GLIB_TOP)/lib \
	-L$(WIN32_GTK_TOP)/lib \
	-L$(PIDGIN_TREE_TOP)/libpurple \
	-L$(PIDGIN_TREE_TOP)/pidgin \
	-lgtk-win32-2.0 \
	-lgdk-win32-2.0 \
	-lgobject-2.0 \
	-lglib-2.0 \
	-lgdk_pixbuf-2.0 \
	-lpurple \
	-lpidgin \
	-static-libgcc

C_FILES = gif_loop_limit.c

.PHONY: all install FAILNOPIDGIN clean

all: $(PLUGIN_TARGET)

gif_loop_limit.so: $(C_FILES)
	$(CC) -fPIC $(CFLAGS) -shared -o $@ $^ $(LDFLAGS) `$(PKG_CONFIG) pidgin purple glib-2.0 gtk+-2.0 gdk-pixbuf-2.0 --libs --cflags` $(INCLUDES)

gif_loop_limit.dll: $(C_FILES)
	$(WIN32_CC) -shared -o $@ $^ $(WIN32_CFLAGS) $(WIN32_LDFLAGS)

install: $(PLUGIN_TARGET)
	mkdir -m $(DIR_PERM) -p $(PLUGIN_DEST)
	install -m $(LIB_PERM) -p $(PLUGIN_TARGET) $(PLUGIN_DEST)

FAILNOPIDGIN:
	@echo "You need pidgin and libpurple development headers installed to compile this plugin"

clean:
	rm -f gif_loop_limit.so gif_loop_limit.dll
