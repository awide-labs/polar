#include "px_optimizer_util/utils/px_defs.h" /* to prevent list_sort be mangled */
#include "px_optimizer_util/px_wrappers.h"
#include "fmgr.h"
#include <stdlib.h>

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

extern "C"
{

PG_FUNCTION_INFO_V1(test_px_partition_child_list);
Datum
test_px_partition_child_list(PG_FUNCTION_ARGS)
{
    Oid oid = PG_GETARG_OID(0);
    /*
     * The name of a wrapper seems incorrect, because it returns child
     * partitions oids not only for index relation.
     */
    List *partition_oid_list = px::GetRelChildIndexes(oid);
    char* oid_array = (char*) palloc0(512);
    ListCell *lc;
    sprintf(oid_array, "{");
    list_sort(partition_oid_list, list_oid_cmp);
    foreach (lc, partition_oid_list)
    {
        char oid_str[256];
        sprintf(oid_str, "%d", lfirst_oid(lc));
        strcat(oid_array, oid_str);
        if (lnext(partition_oid_list, lc))
            strcat(oid_array, ", ");
    }
    strcat(oid_array, "}");
    PG_RETURN_TEXT_P(cstring_to_text(oid_array));
}

PG_FUNCTION_INFO_V1(test_px_partition_is_partition_rel);
Datum
test_px_partition_is_partition_rel(PG_FUNCTION_ARGS)
{
    Oid oid = PG_GETARG_OID(0);
    PG_RETURN_BOOL(px::RelIsPartitioned(oid));
}

PG_FUNCTION_INFO_V1(test_px_partition_is_partition_index);
Datum
test_px_partition_is_partition_index(PG_FUNCTION_ARGS)
{
    Oid oid = PG_GETARG_OID(0);
    PG_RETURN_BOOL(px::IndexIsPartitioned(oid));
}

PG_FUNCTION_INFO_V1(test_px_partition_funcs);
Datum
test_px_partition_funcs(PG_FUNCTION_ARGS)
{
    Oid rel_oid = PG_GETARG_OID(0);
    bool is_partitioned = PG_GETARG_BOOL(1);
    List *list_rels = NIL;
    List *list_index = NIL;
    int level = 0;

    /* clear data in cache */
    RelationForgetRelation(rel_oid);

    Relation rel = RelationIdGetRelation(rel_oid);
    PartitionDesc desc = NULL;
    PartitionKey part_key = NULL;

    Assert(rel->rd_partdesc == NULL);

    px::GetPartTableHegiht(rel_oid, &level, 0);
    px::GetRelLeafPartTables(rel_oid, &list_rels);
    px::GetRelLeafPartIndexes(rel_oid, &list_index);

    if (is_partitioned)
    {
        desc = px::GetPartitionDesc(rel);
        Assert(desc != NULL);
        Assert(rel->rd_partkey != NULL);
        Assert(level > 0);
        Assert(list_length(list_rels) > 1);
        Assert(list_length(list_index) >  0);

        /* Check the key is the same, that was created by GetPartitionDesc */
        part_key = px::GetPartitionKey(rel);
        Assert(part_key == rel->rd_partkey);
    }
    else
    {
        Assert(desc == NULL);
        Assert(rel->rd_partkey == NULL);
        Assert(level == 0);
        Assert(list_length(list_rels) == 1);
        Assert(list_length(list_index) == 1);
    }

    RelationClose(rel);
    PG_RETURN_BOOL(true);
}

}
