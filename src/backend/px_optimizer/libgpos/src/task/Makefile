#
# Makefile for optimizer
#
# src/backend/px_optimizer/px_optimizer/libgpos/src/task/Makefile
#

subdir = src/backend/px_optimizer/libgpos/src/task
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CAutoSuspendAbort.o \
              CAutoTaskProxy.o \
              CAutoTraceFlag.o \
              CTask.o \
              CTaskContext.o \
              CTaskLocalStorage.o \
              CTaskSchedulerFifo.o \
              CWorker.o \
              CWorkerPoolManager.o \
              ITask.o \
              IWorker.o

include $(top_srcdir)/src/backend/common.mk

