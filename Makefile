#
# Plugin Makefile generated by Psi4.
#
# You shouldn't need to modify anything in this file.
#

# Location of your PSI4 source
top_srcdir = /Users/francesco/Source/psi4
# Location of your PSI4 install, by default as listed
top_objdir = /Users/francesco/Source/psi4-github-compile-c++11-debug

# Start by figuring out whether we're on Linux or Mac (sorry, Mr. Gates)
UNAME := $(shell uname)

include $(top_objdir)/src/bin/MakeVars

# Reset these values, MakeVars changes them to valud only valid in Psi4's objdir
# Location of your PSI4 source
top_srcdir = /Users/francesco/Source/psi4
# Location of your PSI4 install, by default as listed
top_objdir = /Users/francesco/Source/psi4-github-compile-c++11-debug

PSITARGET = $(shell basename `pwd`).so
#PSILIBS = -L$(top_objdir)/lib -lPSI_plugin -l/Users/francesco/Source/libbtl/libbtl.so # -lbtl
PSILIBS = -L$(top_objdir)/lib -lPSI_plugin
LIBS += -L/Users/francesco/Source/libbtl -lbtl

CXXSRC = $(notdir $(wildcard *.cc))
CXXFLAGS += -I/Users/francesco/Source/libbtl
CXXFLAGS += -D_HAS_LIBBTL_
DEPENDINCLUDE = $(notdir $(wildcard *.h))

BINOBJ = $(CXXSRC:%.cc=%.o)

default:: $(PSITARGET)

# Add the flags needed for shared library creation
ifeq ($(UNAME), Linux)
    LDFLAGS = -shared
endif
ifeq ($(UNAME), Darwin)
    LDFLAGS = -shared -undefined dynamic_lookup
    CXXOTH += -fno-common
endif

# The object files
%.o: %.cc
	$(CXX) $(CXXFLAGS) $(CXXINCLUDE) -c $< $(OUTPUT_OPTION)

$(PSITARGET): $(BINOBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(CXXDBG) $(PSILIBS)

# Erase all compiled intermediate files
clean:
	rm -f $(BINOBJ) $(PSITARGET) *.d *.pyc *.test output.dat psi.timer.dat

# Dependency handling
%.d: %.cc
	$(CXXDEPEND) $(CXXDEPENDFLAGS) $(CXXFLAGS) $(CXXINCLUDE) $< | sed 's/^$*.o/$*.o $*.d/g' > $(@F)

ifneq ($(DODEPEND),no)
$(BINOBJ:%.o=%.d): $(DEPENDINCLUDE)
include $(BINOBJ:%.o=%.d)
endif

