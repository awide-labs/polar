/*-------------------------------------------------------------------------
 *
 * faultinjector.h
 *	  Definitions for fault based testing framework.
 *
 * Copyright 2009-2010, Greenplum Inc. All rights reserved.
 *
 * src/include/utils/faultinjector.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FAULTINJECTOR_H
#define FAULTINJECTOR_H

#include "pg_config_manual.h"

#define FAULTINJECTOR_MAX_SLOTS	16

#define FAULT_NAME_MAX_LENGTH	256

#define INFINITE_END_OCCURRENCE -1

/* Fault name that matches all faults */
#define FaultInjectorNameAll "all"

typedef enum FaultInjectorType_e
{
#define FI_TYPE(id, str) id,
#include "utils/faultinjector_lists.h"
#undef FI_TYPE
	FaultInjectorTypeMax
}			FaultInjectorType_e;

/*
 *
 */
typedef enum FaultInjectorState_e
{
#define FI_STATE(id, str) id,
#include "utils/faultinjector_lists.h"
#undef FI_STATE
	FaultInjectorStateMax
}			FaultInjectorState_e;


/*
 *
 */
typedef struct FaultInjectorEntry_s
{

	char		faultName[FAULT_NAME_MAX_LENGTH];

	FaultInjectorType_e faultInjectorType;

	int			extraArg;
	/* in seconds, in use if fault injection type is sleep */

	char		databaseName[NAMEDATALEN];

	char		tableName[NAMEDATALEN];

	volatile int startOccurrence;
	volatile int endOccurrence;
	volatile int numTimesTriggered;
	volatile	FaultInjectorState_e faultInjectorState;

	/* the state of the fault injection */
	char		bufOutput[2500];

}			FaultInjectorEntry_s;


extern Size FaultInjector_ShmemSize(void);

extern void FaultInjector_ShmemInit(void);

extern FaultInjectorType_e FaultInjector_TriggerFaultIfSet(
														   const char *faultName,
														   const char *databaseName,
														   const char *tableName);

extern char *InjectFault(
						 char *faultName, char *type, char *databaseName, char *tableName,
						 int startOccurrence, int endOccurrence, int extraArg);

#ifdef FAULT_INJECTOR
#define SIMPLE_FAULT_INJECTOR(FaultName) \
	FaultInjector_TriggerFaultIfSet(FaultName, "", "")
#else
#define SIMPLE_FAULT_INJECTOR(FaultName)
#endif

#endif							/* FAULTINJECTOR_H */
