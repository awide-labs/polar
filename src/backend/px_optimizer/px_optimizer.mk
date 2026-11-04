override CPPFLAGS := -I$(top_srcdir)/src/backend/px_optimizer/libgpos/include $(CPPFLAGS)
override CPPFLAGS := -I$(top_srcdir)/src/backend/px_optimizer/libgpopt/include $(CPPFLAGS)
override CPPFLAGS := -I$(top_srcdir)/src/backend/px_optimizer/libnaucrates/include $(CPPFLAGS)
override CPPFLAGS := -I$(top_srcdir)/src/backend/px_optimizer/libgpdbcost/include $(CPPFLAGS)
# Do not omit frame pointer. Even with RELEASE builds, it is used for
# backtracing.
override CPPFLAGS := -std=c++11 $(CPPFLAGS)
override CXXFLAGS := -std=c++11 -Werror -Wextra -Wpedantic -Wno-variadic-macros -fno-omit-frame-pointer $(CXXFLAGS)
