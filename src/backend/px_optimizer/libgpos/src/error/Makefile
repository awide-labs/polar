#
# Makefile for optimizer
#
# src/backend/px_optimizer/libgpos/src/error/Makefile
#

subdir = src/backend/px_optimizer/libgpos/src/error
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CAutoExceptionStack.o \
              CAutoTrace.o \
              CErrorContext.o \
              CErrorHandlerStandard.o \
              CException.o \
              CLogger.o \
              CLoggerStream.o \
              CLoggerSyslog.o \
              CMessage.o \
              CMessageRepository.o \
              CMessageTable.o \
              CMiniDumper.o \
              CSerializable.o \
              ILogger.o

include $(top_srcdir)/src/backend/common.mk

