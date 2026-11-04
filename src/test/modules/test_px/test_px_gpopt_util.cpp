/* test px_optimizer_util/cpp */
#include <stdint.h>
namespace gpos
{
    typedef uint32_t ULONG;
    typedef bool BOOL;
}

#include "px_optimizer_util/CGPOptimizer.h"
#include "px_optimizer_util/px_wrappers.h"

#include "postgres.h"
#include "fmgr.h"

/*
 * PG_FUNCTION_INFO_V1 ends with "extern int no_such_variable", which clashes
 * in C++ after catalog headers from px_wrappers.h are included.
 * Keep behavior but drop that trailing declaration for this file.
 */
#ifdef __cplusplus
#undef PG_FUNCTION_INFO_V1
#define PG_FUNCTION_INFO_V1(funcname) \
extern Datum funcname(PG_FUNCTION_ARGS); \
extern PGDLLEXPORT const Pg_finfo_record * CppConcat(pg_finfo_,funcname)(void); \
const Pg_finfo_record * \
CppConcat(pg_finfo_,funcname) (void) \
{ \
    static const Pg_finfo_record my_finfo = { 1 }; \
    return &my_finfo; \
}
#endif

extern "C" {

/* test px_wrappers*/
PG_FUNCTION_INFO_V1(test_px_gpopt_wrapper);
Datum
test_px_gpopt_wrapper(PG_FUNCTION_ARGS)
{
    /* datum from */
    {
        px::DatumFromUint8(0);
        px::DatumFromInt16(0);
        px::DatumFromUint16(0);
        px::DatumFromUint32(0);
        px::DatumFromPointer(0);
    }
    /* from datum */
    {
        Datum d;
        d = 0;
        px::Int16FromDatum(d);
        px::Uint16FromDatum(d);
        px::lUint32FromDatum(d);
        px::OidFromDatum(d);
        px::PointerFromDatum(d);
        px::Float4FromDatum(d);
        px::Float8FromDatum(d);
    }
    /* Oid*/
    try
    {
        void * ptr;
        List *list_int, *list_oid;
        StringInfo s_info;
        list_int = ListMake1Int(0);
        list_oid = ListMake1Oid(0);
        px::FuncExecLocation(0);
        // px::GetTriggerName(0);
        // px::GetTriggerRelid(0);
        // px::GetTriggerFuncid(0);
        // px::GetTriggerType(0);
        // px::IsTriggerEnabled(0);
        // px::TriggerExists(0);
        // px::GetRelationPartContraints(0, NULL);
        // px::GetRootPartition(0);
        // px::GetFuncArgTypes(0);
        // px::GetPartitionAttrs(0);
        // px::IsLegacyPxHashFunction(0);
        // px::GetLegacyPxHashOpclassForBaseType(0);

        /*list*/
        px::LPrependOid(0, NULL);
        px::LPrependOid(0, list_oid);
        px::ListCopy(list_int);
        px::ListNthInt(list_int, 0);
        px::ListMemberOid(list_oid, 0);

        px::MakeIntegerValue(0);
        // px::MakeNULLConst(0);
        ptr = palloc(1);
        ptr = px::MemCtxtRealloc(ptr, 10);
        pfree(ptr);
        // px::RelPartIsRoot(0);
        // px::RelPartIsInterior(0);
        // px::RelPartIsNone(0);
        // px::HasSubclassSlow(0);
        // px::ChildPartHasTriggers(0,0);
        // px::GetLogicalPartIndexes(0);
        // px::BuildRelationTriggers(0);

        // px::GetIntFromValue(0);
        // px::GetComponentDatabases();
        px::StrCmpIgnoreCase("a", "b");
        px::ConstructRandomSegMap(10,2);
        s_info = px::MakeStringInfo();
        px::AppendStringInfo(s_info, "a", "b");
        // px::CountLeafPartTables(0);
        px::MakePxPolicy(POLICYTYPE_PARTITIONED,1,1);
    }
    catch(...)
    {

    }
    PG_RETURN_VOID();
}

} /*extern "C"*/
