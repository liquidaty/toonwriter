# Makefile for use with GNU make

THIS_MAKEFILE_DIR:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
THIS_DIR:=$(shell basename "${THIS_MAKEFILE_DIR}")
THIS_MAKEFILE:=$(lastword $(MAKEFILE_LIST))

.POSIX:
.SUFFIXES:
.SUFFIXES: .o .c .a

CONFIGFILE ?= config.mk
$(info Using config file ${CONFIGFILE})
include ${CONFIGFILE}

CC ?= cc
AWK ?= awk
AR ?= ar
RANLIB ?= ranlib
SED ?= sed

WIN=
DEBUG=0
ifeq ($(WIN),)
  WIN=0
  ifneq ($(findstring w64,$(CC)),) # e.g. mingw64
    WIN=1
  endif
endif

CFLAGS+=${CFLAG_O} ${CFLAGS_OPT}
CFLAGS+=${CFLAGS_AUTO}
CFLAGS+=-I.

ifeq ($(VERBOSE),1)
  CFLAGS+= ${CFLAGS_VECTORIZE_OPTIMIZED} ${CFLAGS_VECTORIZE_MISSED} ${CFLAGS_VECTORIZE_ALL}
endif

VERSION= $(shell (git describe --always --dirty --tags 2>/dev/null || echo "v0.0.0-toonwriter") | sed 's/^v//')

ifneq ($(findstring emcc,$(CC)),) # emcc
  NO_THREADING=1
endif

ifeq ($(NO_THREADING),1)
  CFLAGS+= -DNO_THREADING
endif

ifeq ($(ASAN),1)
  CFLAGS+= -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -UNDEBUG
  LDFLAGS+= -fsanitize=address,undefined
else ifeq ($(DEBUG),0)
  CFLAGS+= -DNDEBUG -O3  ${CFLAGS_LTO}
else
  CFLAGS += ${CFLAGS_DEBUG}
endif

ifeq ($(ASAN),1)
  DBG_SUBDIR=asan
else ifeq ($(DEBUG),1)
  DBG_SUBDIR=dbg
else
  DBG_SUBDIR=rel
endif

ifeq ($(WIN),0)
  BUILD_SUBDIR=$(shell uname)/${DBG_SUBDIR}
  WHICH=which
  EXE=
  CFLAGS+= -fPIC
else
  BUILD_SUBDIR=win/${DBG_SUBDIR}
  WHICH=where
  EXE=.exe
  CFLAGS+= -fpie
  CFLAGS+= -D__USE_MINGW_ANSI_STDIO -D_ISOC99_SOURCE -Wl,--strip-all
endif

CFLAGS+= -std=gnu11 -Wno-gnu-statement-expression -Wshadow -Wall -Wextra -Wno-missing-braces -pedantic -D_GNU_SOURCE

CFLAGS+= ${TOONWRITER_OPTIONAL_CFLAGS}

CCBN=$(shell basename ${CC})
THIS_LIB_BASE=$(shell cd .. && pwd)
INCLUDE_DIR=${THIS_LIB_BASE}/include
BUILD_DIR=${THIS_LIB_BASE}/build/${BUILD_SUBDIR}/${CCBN}

NO_UTF8_CHECK=1

LIB_SUFFIX?=
TOONWRITER_OBJ=${BUILD_DIR}/objs/toonwriter.o
LIBTOONWRITER_A=libtoonwriter${LIB_SUFFIX}.a
LIBTOONWRITER=${BUILD_DIR}/lib/${LIBTOONWRITER_A}
LIBTOONWRITER_INSTALL=${LIBDIR}/${LIBTOONWRITER_A}

TOONWRITER_OBJ_OPTS=
ifeq ($(NO_UTF8_CHECK),1)
  TOONWRITER_OBJ_OPTS+= -DNO_UTF8_CHECK
endif

help:
	@echo "Make options:"
	@echo "  `basename ${MAKE}` build|install|uninstall|clean"
	@echo
	@echo "Optional make variables:"
	@echo "  [CONFIGFILE=config.mk] [NO_UTF8_CHECK=1] [VERBOSE=1] [LIBDIR=${LIBDIR}] [INCLUDEDIR=${INCLUDEDIR}] [LIB_SUFFIX=]"
	@echo

build: ${LIBTOONWRITER}

# Print the resolved per-config build directory (used by CI to locate the
# cross-compiled / wasm test and fuzz binaries that it cannot simply `make test`).
print-builddir:
	@echo ${BUILD_DIR}

${LIBTOONWRITER}: ${TOONWRITER_OBJ}
	@mkdir -p `dirname "$@"`
	@rm -f $@
	$(AR) rcv $@ $?
	$(RANLIB) $@
	$(AR) -t $@ # check it is there
	@echo Built $@

install: ${LIBTOONWRITER_INSTALL}
	@mkdir -p  $(INCLUDEDIR)
	@cp -pR toonwriter.h $(INCLUDEDIR)
	@echo "include files copied to $(INCLUDEDIR)"

${LIBTOONWRITER_INSTALL}: ${LIBTOONWRITER}
	@mkdir -p `dirname "$@"`
	@cp -p ${LIBTOONWRITER} "$@"
	@echo "libtoonwriter installed to $@"

uninstall:
	@rm -rf ${INCLUDEDIR}/toonwriter*
	 rm  -f ${LIBDIR}/libtoonwriter*

clean:
	rm -rf ${BUILD_DIR}/objs ${LIBTOONWRITER}

.PHONY: build install uninstall clean test check test-leaks strict print-builddir fuzz fuzz-standalone ${LIBTOONWRITER_INSTALL}

${BUILD_DIR}/objs/toonwriter.o: toonwriter.c toonwriter.h toon_numeric.c toon_numeric.h
	@mkdir -p `dirname "$@"`
	${CC} ${CFLAGS} -DINCLUDE_UTILS -DTOONWRITER_VERSION=\"${VERSION}\" -I${INCLUDE_DIR} ${TOONWRITER_OBJ_OPTS} -o $@ -c $<

# ---------------------------------------------------------------- tests / fuzz
TEST_BIN=${BUILD_DIR}/bin/test${EXE}
FUZZ_STANDALONE_BIN=${BUILD_DIR}/bin/fuzz-standalone${EXE}
FUZZ_BIN=${BUILD_DIR}/bin/fuzz${EXE}
FUZZ_CC?=clang

test: ${TEST_BIN}
	${TEST_BIN}

check: test

# Strict-warnings lane (not part of the default build): syntax-check the library
# translation unit under an explicit standard with extra diagnostics promoted to
# errors. Catches implicit narrowing / shadowing the default build tolerates.
# Run across compilers, e.g. `make strict CC=gcc`.
STRICT_WARN= -std=c11 -pedantic -Wall -Wextra -Werror \
             -Wconversion -Wshadow -Wstrict-prototypes -Wvla
strict:
	${CC} ${STRICT_WARN} -I. -I${INCLUDE_DIR} -DINCLUDE_UTILS \
	  -DTOONWRITER_VERSION=\"${VERSION}\" ${TOONWRITER_OBJ_OPTS} \
	  -fsyntax-only toonwriter.c
	@echo "strict: clean (${CC})"

${TEST_BIN}: test.c toonwriter.h ${LIBTOONWRITER}
	@mkdir -p `dirname "$@"`
	${CC} ${CFLAGS} -I. -o $@ test.c ${LIBTOONWRITER} ${LDFLAGS}

# Leak check: prefer macOS 'leaks'; otherwise run the suite directly (ASAN=1
# builds catch leaks via LeakSanitizer on platforms that support it).
test-leaks: ${TEST_BIN}
	@if command -v leaks >/dev/null 2>&1; then \
	  echo "leaks: ${TEST_BIN}"; \
	  MallocStackLogging=1 leaks --atExit -- ${TEST_BIN}; \
	else echo "leaks(1) not found; running suite directly"; ${TEST_BIN}; fi

# Portable replay driver: builds with any toolchain (no libFuzzer needed).
fuzz-standalone: ${FUZZ_STANDALONE_BIN}
${FUZZ_STANDALONE_BIN}: fuzz.c toonwriter.h ${LIBTOONWRITER}
	@mkdir -p `dirname "$@"`
	${CC} ${CFLAGS} -DTOONW_FUZZ_STANDALONE -I. -o $@ fuzz.c ${LIBTOONWRITER} ${LDFLAGS}

# libFuzzer build (needs an LLVM clang with libFuzzer; Apple clang lacks it).
# Compiles the library TU with the harness so instrumentation reaches it.
fuzz:
	@mkdir -p ${BUILD_DIR}/bin
	${FUZZ_CC} -std=gnu11 -I. -g -O1 -fno-omit-frame-pointer \
	  -fsanitize=fuzzer,address,undefined -o ${FUZZ_BIN} fuzz.c toonwriter.c
	@echo "built ${FUZZ_BIN}"
	@echo "run e.g.: ${FUZZ_BIN} -max_total_time=60"
