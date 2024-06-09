.SUFFIXES: .cpp
CXX?=g++
CXXFLAGS?=-march=native -O3 -std=gnu++26
RM?=rm -f
INSTALL?=install
INSTALL_DIR     = $(INSTALL) -p -d -m  755
INSTALL_PROGRAM = $(INSTALL) -p    -m  755
INSTALL_DATA    = $(INSTALL) -p    -m  644

prefix ?= /usr
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
man1dir?= $(prefix)/share/man/man1

export
all: unlzx$(EXEEXT)

unlzx$(EXEEXT): unlzx.cpp
	$(CXX) $(CXXFLAGS) unlzx.cpp -o unlzx$(EXEEXT)
clean:
	-$(RM) unlzx$(EXEEXT)

install: unlzx$(EXEEXT)
	 $(INSTALL_DIR)  $(DESTDIR)$(bindir)
	 $(INSTALL_DIR)  $(DESTDIR)$(man1dir)
	 $(INSTALL_PROGRAM) unlzx$(EXEEXT) $(DESTDIR)$(bindir)

uninstall:
	 $(RM) $(DESTDIR)$(bindir)/unlzx$(EXEEXT)
	 $(RM) $(DESTDIR)$(man1dir)/unlzx.1
