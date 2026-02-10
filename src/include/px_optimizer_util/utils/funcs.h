//---------------------------------------------------------------------------
//
// funcs.h
//    API for invoking optimizer using GPDB udfs
//
// Copyright (c) 2019-Present VMware, Inc. or its affiliates.
//
//---------------------------------------------------------------------------

#ifndef PXOPT_funcs_H
#define PXOPT_funcs_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

extern Datum DisableXform(PG_FUNCTION_ARGS);
extern Datum EnableXform(PG_FUNCTION_ARGS);

// pg15_px: TODO: remove or return valid version
#if 0
extern Datum LibraryVersion();
#endif

#ifdef __cplusplus
}
#endif

#endif	// PXOPT_funcs_H
