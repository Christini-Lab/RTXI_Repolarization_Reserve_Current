PLUGIN_NAME = reverse_repolarization_current

HEADERS = RRC.h \
	RRC_MainWindow_UI.h

SOURCES = RRC.cpp moc_RRC.cpp

LIBS = -lgsl -lgslcblas -lrtmath

### Do not edit below this line ###

include $(shell rtxi_plugin_config --pkgdata-dir)/Makefile.plugin_compile

RRC.cpp: RRC_MainWindow_UI.h

RRC_MainWindow_UI.h: RRC_MainWindow.ui
	uic RRC_MainWindow.ui -o RRC_MainWindow_UI.h
