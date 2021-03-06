# configuration for hfsm sample implementasion.

NAME = hfsm
MAJOR_VERSION = 1
MINOR_VERSION = 0
REVISION = 0
VERSION = $(MAJOR_VERSION).$(MINOR_VERSION).$(REVISION)

NODEBUG = 0

## Header direcotyr of Catch2 test framework.
CATCH2_DIR ?=

## Doxygen options.
DOXY_PROJECT = "HFSM: Hierchical Finite State Machine"
DOXY_BRIEF = "HFSM: Hierchical Finite State Machine Implementation"
DOXY_OUTPUT = doxygen
DOXY_SOURCES = include src

CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
LD = $(CROSS_COMPILE)ld

V = 0
Q1 = $(V:1=)
QCC    = $(Q1:0=@echo '    CC   ' $@;)
QCXX   = $(Q1:0=@echo '    CXX  ' $@;)
QLD    = $(Q1:0=@echo '    LD   ' $@;)
QLINK  = $(Q1:0=@echo '    LINK ' $@;)
QCLEAN = $(Q1:0=@echo '    CLEAN';)
