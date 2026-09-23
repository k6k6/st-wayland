# st version
VERSION = 0.9.3

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man
ICONPREFIX = $(PREFIX)/share/pixmaps
ICONNAME = st.png

PKG_CONFIG = pkg-config
WAYLAND_SCANNER = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

# Uncomment the lines below for the ligatures patch / LIGATURES_PATCH
LIGATURES_C = hb.c
LIGATURES_H = hb.h
LIGATURES_INC = `$(PKG_CONFIG) --cflags harfbuzz`
LIGATURES_LIBS = `$(PKG_CONFIG) --libs harfbuzz`

# Uncomment this for the SIXEL patch / SIXEL_PATCH
SIXEL_C = sixel.c sixel_hls.c

# netwmicon patch: on Wayland the icon is resolved from the desktop entry
# (app_id), no image library is needed.
NETWMICON_LIBS =

# includes and libs
INCS = `$(PKG_CONFIG) --cflags wayland-client wayland-cursor xkbcommon cairo cairo-ft` \
       `$(PKG_CONFIG) --cflags fontconfig` \
       `$(PKG_CONFIG) --cflags freetype2` \
       $(LIGATURES_INC)
LIBS = -lm -lutil -lrt \
       `$(PKG_CONFIG) --libs wayland-client wayland-cursor xkbcommon cairo cairo-ft` \
       `$(PKG_CONFIG) --libs fontconfig` \
       `$(PKG_CONFIG) --libs freetype2` \
       $(LIGATURES_LIBS) \
       $(NETWMICON_LIBS)

# flags
CFLAGS = -O2 -march=native -flto
LDFLAGS = -flto
STCPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE
STCFLAGS = $(INCS) $(STCPPFLAGS) $(CPPFLAGS) $(CFLAGS)
STLDFLAGS = $(LIBS) $(LDFLAGS)

# OpenBSD:
#CPPFLAGS = $(STCPPFLAGS) -D_XOPEN_SOURCE=600
#MANPREFIX = ${PREFIX}/man

# compiler and linker
# CC = c99
