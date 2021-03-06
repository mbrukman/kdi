MAGIC_REQUIRE_VARS := HYPERTABLE_HOME

MAGIC_FLAGS_CPPFLAGS := -I$(HYPERTABLE_HOME)/include
vpath %.a $(HYPERTABLE_HOME)/lib
vpath %.so $(HYPERTABLE_HOME)/lib

MAGIC_MODULE_DEPS := kdi
MAGIC_EXTERNAL_DEPS := Hypertable_PREFER_STATIC Hyperspace_PREFER_STATIC HyperCommon_PREFER_STATIC HyperComm_PREFER_STATIC log4cpp $(BOOST_IOSTREAMS)
include magic.mk
