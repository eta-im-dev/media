HEADERS += \
	$$PWD/osxvideo/osxvideosink.h \
	$$PWD/osxvideo/cocoawindow.h \
	$$PWD/osxvideo/osxvideosrc.h

SOURCES += \
	$$PWD/osxvideo/osxvideosink.m \
	$$PWD/osxvideo/cocoawindow.m \
	$$PWD/osxvideo/osxvideosrc.c

gstplugin:SOURCES += $$PWD/osxvideo/osxvideoplugin.m
!gstplugin:SOURCES += $$PWD/static/osxvideo_static.m

LIBS *= \
	-lgstinterfaces-0.10 \
	-lgstvideo-0.10

LIBS += \
	-framework Cocoa \
	-framework QuickTime \
	-framework OpenGL
