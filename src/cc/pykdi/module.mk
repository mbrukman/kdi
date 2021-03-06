MAGIC_FLAGS_CPPFLAGS := $(PYTHON_INCLUDE)

MAGIC_MODULE_DEPS := kdi_meta kdi_net kdi_hash
MAGIC_EXTERNAL_DEPS := $(BOOST_PYTHON)
include magic.mk


# Special stuff for Python module
$(build)/pykdi.so: $(_LIB_SO)
	@echo "Alias $< as $@"
	@ln -nf $< $@

TARGETS := $(TARGETS) $(build)/pykdi.so
LIB_INSTALL :=
PY_INSTALL := $(build)/pykdi.so
