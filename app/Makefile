# The application that is statically linked into the kernel image.
#
# The top-level Makefile includes this file for every value of `app` except
# `tests`, then links whatever ends up in $(app-objects). Only one application
# can be linked at a time: the kernel enters it through a single
# osv_app_main(), so two of them would collide at link time.
#
#   make              -> app/app.cc, the placeholder below
#   make app=duckdb   -> DuckDB      (the app/miniduckdb submodule)
#   make app=llama    -> llama.cpp   (the app/llama.cpp submodule)
#   make app=tests    -> the test suite in test/ (see the top-level Makefile)
#
# Each submodule ships its own fragment at miniosv/miniosv.mk, which sets
# $(app-objects) and whatever extra compile rules its sources need. The paths
# there are relative to the miniOSv root, not to the submodule.

ifeq ($(app),default)
app-objects = app/app.o
else ifeq ($(app),duckdb)
app-fragment = app/miniduckdb/miniosv/miniosv.mk
else ifeq ($(app),llama)
app-fragment = app/llama.cpp/miniosv/miniosv.mk
else
$(error unknown app '$(app)': expected default, duckdb, llama or tests)
endif

ifdef app-fragment
ifeq ($(wildcard $(app-fragment)),)
$(error $(app-fragment) is missing: check out the submodules with \
        'git submodule update --init', and note that the port itself may not be wired up yet)
endif
include $(app-fragment)
endif
