/*-------------------------------------------------------------------------
 *
 * pg_jieba.c
 *    a text search parser for Chinese
 *
 * Author: Jaimin Pan <jaimin.pan@gmail.com>
 *
 * IDENTIFICATION
 *    pg_jieba.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <string>
#include <vector>

#include "cppjieba/Jieba.hpp"

#ifdef __cplusplus
extern "C" {
#endif

#include "postgres.h"
#include "utils/guc.h"
#include "utils/elog.h"
#include "miscadmin.h"
#include "tsearch/ts_public.h"
#include "executor/spi.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/ipc.h"

#include "sys/stat.h"
#include "unistd.h"
#include "fcntl.h"

#ifdef __cplusplus
}
#endif

namespace pg_jieba {

using namespace cppjieba;
using namespace std;

typedef struct
{
  LWLock *lock;
} jiebaSharedState;

static int jieba_char_position = 0;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static jiebaSharedState *jieba_status = NULL;
static const char* const tok_alias[] = {
  "",
  "nz",
  "n",
  "m",
  "i",
  "l",
  "d",
  "s",
  "t",
  "mq",
  "nr",
  "j",
  "a",
  "r",
  "b",
  "f",
  "nrt",
  "v",
  "z",
  "ns",
  "q",
  "vn",
  "c",
  "nt",
  "u",
  "o",
  "zg",
  "nrfg",
  "df",
  "p",
  "g",
  "y",
  "ad",
  "vg",
  "ng",
  "x",
  "ul",
  "k",
  "ag",
  "dg",
  "rr",
  "rg",
  "an",
  "vq",
  "e",
  "uv",
  "tg",
  "mg",
  "ud",
  "vi",
  "vd",
  "uj",
  "uz",
  "h",
  "ug",
  "rz"
};

static const char* const lex_descr[] = {
  "",
  "other proper noun",
  "noun",
  "numeral",
  "idiom",
  "temporary idiom",
  "adverb",
  "space",
  "time",
  "numeral-classifier compound",
  "person's name",
  "abbreviate",
  "adjective",
  "pronoun",
  "difference",
  "direction noun",
  "nrt",
  "verb",
  "z",
  "location",
  "quantity",
  "vn",
  "conjunction",
  "organization",
  "auxiliary",
  "onomatopoeia",
  "zg",
  "nrfg",
  "df",
  "prepositional",
  "morpheme",
  "modal verbs",
  "ad",
  "vg",
  "ng",
  "unknown",
  "ul",
  "k",
  "ag",
  "dg",
  "rr",
  "rg",
  "an",
  "vq",
  "exclamation",
  "uv",
  "tg",
  "mg",
  "ud",
  "vi",
  "vd",
  "uj",
  "uz",
  "h",
  "ug",
  "rz"
};

/*
 * types
 */
typedef struct
{
  vector<string>::iterator iter;
  vector<string>* words;
} JiebaCtx;

#ifdef __cplusplus
extern "C" {
#endif
PG_MODULE_MAGIC;

void    _PG_init(void);
#if (PG_VERSION_NUM >= 150000)
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void  jieba_shmem_request(void);
#else
void    _PG_fini(void);
#endif
/*
 * prototypes
 */
PG_FUNCTION_INFO_V1(jieba_start);
Datum jieba_start(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(jieba_query_start);
Datum jieba_query_start(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(jieba_gettoken);
Datum jieba_gettoken(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(jieba_end);
Datum jieba_end(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(jieba_lextype);
Datum jieba_lextype(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(jieba_gettoken_with_position);
Datum jieba_gettoken_with_position(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(jieba_load_user_dict);
Datum jieba_load_user_dict(PG_FUNCTION_ARGS);

#ifdef __cplusplus
}
#endif

/* wrapper class for all we used */
class PgJieba {
public:
  PgJieba(const string& dict_path,
          const string& model_path,
          const string& user_dict_path)
    : dict_trie_(dict_path, user_dict_path),
      hmm_model_(model_path),
      mix_seg_(&dict_trie_, &hmm_model_),
      query_seg_(&dict_trie_, &hmm_model_) {
    auto num_types = sizeof(tok_alias) / sizeof(tok_alias[0]);
    for (auto i = 1; i < num_types; ++i) {
      lex_id_.insert({tok_alias[i], i});
    }
  }

  void Cut(const string& sentence, vector<string>& words, bool hmm = true) const {
    mix_seg_.Cut(sentence, words, hmm);
  }
  void Cut(const string& sentence, vector<Word>& words, bool hmm = true) const {
    mix_seg_.Cut(sentence, words, hmm);
  }

  void CutForSearch(const string& sentence, vector<string>& words, bool hmm = true) const {
    query_seg_.Cut(sentence, words, hmm);
  }
  void CutForSearch(const string& sentence, vector<Word>& words, bool hmm = true) const {
    query_seg_.Cut(sentence, words, hmm);
  }

  string LookupTag(const string &str) const {
    return mix_seg_.LookupTag(str);
  }

  int LookupLexTypeId(const string &str) const {
    try {
      return lex_id_.at(this->LookupTag(str));
    } catch(...) {
      return lex_id_.at("n");
    }
  }

  int GetLexTypeSize() const {
    return lex_id_.size();
  }

private:
  DictTrie dict_trie_;
  HMMModel hmm_model_;

  // They share the same dict trie and model
  MixSegment mix_seg_;
  QuerySegment query_seg_;
  unordered_map<string, int> lex_id_;
}; // class PgJieba

static PgJieba* jieba = nullptr;
static const char* DICT_PATH = "jieba.dict";
static const char* HMM_PATH = "jieba.hmm_model";
static const char* USER_DICT = "jieba.user.dict";
static const char* EXT = "utf8";

static char* jieba_get_tsearch_config_filename(const char *basename, const char *extension, int num);
static char* jieba_get_user_tsearch_pathname();
static int   jieba_get_utf8_char_number(const char *s);
static void  jieba_shmem_startup(void);

/*
 * Module load callback
 */
void _PG_init(void)
{
  if (jieba != nullptr) {
    return;
  }

  /*
   init will take a few seconds to load dicts.
   */
  jieba = new PgJieba(jieba_get_tsearch_config_filename(DICT_PATH, EXT, -1),
                      jieba_get_tsearch_config_filename(HMM_PATH, EXT, -1),
                      jieba_get_tsearch_config_filename(USER_DICT, EXT, -1));

  /*
  * In order to create our shared memory area, we have to be loaded via
  * shared_preload_libraries.  If not, fall out without hooking into any of
  * the main system.
  * We don't throw error here because users can still use basic functions
  * except custom dictionaries.
  */
  if (!process_shared_preload_libraries_in_progress)
    return;

#if (PG_VERSION_NUM >= 150000)
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = jieba_shmem_request;
#else
    RequestAddinShmemSpace(MAXALIGN(sizeof(jiebaSharedState)));
    RequestNamedLWLockTranche("jieba", 1);
#endif

  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook = jieba_shmem_startup;
}

#if (PG_VERSION_NUM >= 150000)
static void jieba_shmem_request(void)
{
  if (prev_shmem_request_hook)
    prev_shmem_request_hook();
  RequestAddinShmemSpace(MAXALIGN(sizeof(jiebaSharedState)));
  RequestNamedLWLockTranche("jieba", 1);
}
#else
/*
 * Module unload callback
 */
void _PG_fini(void)
{
  if (jieba != nullptr) {
    delete jieba;
    jieba = nullptr;
  }
  shmem_startup_hook = prev_shmem_startup_hook;
}
#endif

/*
 * functions
 */
Datum jieba_start(PG_FUNCTION_ARGS)
{
  string str(static_cast<char*>(PG_GETARG_POINTER(0)),
                 static_cast<unsigned long>(PG_GETARG_INT32(1)));
  auto words = new vector<string>();
  jieba_char_position = 0;
  jieba->Cut(str, *words);

  JiebaCtx* const ctx = static_cast<JiebaCtx*>(palloc0(sizeof(JiebaCtx)));
  ctx->words = words;
  ctx->iter = words->begin();
  PG_RETURN_POINTER(ctx);
}

Datum jieba_query_start(PG_FUNCTION_ARGS)
{
  string str(static_cast<char*>(PG_GETARG_POINTER(0)),
             static_cast<unsigned long>(PG_GETARG_INT32(1)));
  auto words = new vector<string>();
  jieba->CutForSearch(str, *words);

  JiebaCtx* const ctx = static_cast<JiebaCtx*>(palloc0(sizeof(JiebaCtx)));
  ctx->words = words;
  ctx->iter = words->begin();
  PG_RETURN_POINTER(ctx);
}

Datum jieba_gettoken(PG_FUNCTION_ARGS)
{
  JiebaCtx* const ctx = reinterpret_cast<JiebaCtx*>(PG_GETARG_POINTER(0));
  char** t = reinterpret_cast<char**>(PG_GETARG_POINTER(1));
  int* tlen = reinterpret_cast<int*>(PG_GETARG_POINTER(2));
  int type = -1;

  auto& cur_iter = ctx->iter;

  /* already done the work, or no sentence */
  if (cur_iter == ctx->words->end()) {
    *tlen = 0;
    type = 0;

    PG_RETURN_INT32(type);
  }

  type = jieba->LookupLexTypeId(*cur_iter);
  *tlen = static_cast<int>(cur_iter->length());
  *t = const_cast<char*>(cur_iter->c_str());

  ++cur_iter;
  PG_RETURN_INT32(type);
}

Datum jieba_gettoken_with_position(PG_FUNCTION_ARGS)
{
  JiebaCtx* const ctx = reinterpret_cast<JiebaCtx*>(PG_GETARG_POINTER(0));
  char** t = reinterpret_cast<char**>(PG_GETARG_POINTER(1));
  int* tlen = reinterpret_cast<int*>(PG_GETARG_POINTER(2));
  int type = -1;

  auto& cur_iter = ctx->iter;

  /* already done the work, or no sentence */
  if (cur_iter == ctx->words->end()) {
    *tlen = 0;
    type = 0;

    PG_RETURN_INT32(type);
  }

  type = jieba->LookupLexTypeId(*cur_iter);
  *tlen = static_cast<int>(cur_iter->length());


  int position = jieba_get_utf8_char_number(cur_iter->c_str());
  cur_iter->append(":");
  cur_iter->append(to_string(jieba_char_position));
  *t = const_cast<char*>(cur_iter->c_str());
  *tlen += (to_string(jieba_char_position).length() + 1);

  jieba_char_position += position;

  ++cur_iter;
  PG_RETURN_INT32(type);
}

Datum jieba_end(PG_FUNCTION_ARGS)
{
  JiebaCtx* const ctx = reinterpret_cast<JiebaCtx*>(PG_GETARG_POINTER(0));
  if (ctx->words != nullptr) {
    delete ctx->words;
    ctx->words = nullptr;
  }
  pfree(ctx);
  PG_RETURN_VOID();
}

Datum jieba_lextype(PG_FUNCTION_ARGS)
{
  auto size = jieba->GetLexTypeSize();
  /* RDS: change size to size+1, to avoid memory overflow */
  LexDescr *descr = static_cast<LexDescr*>(palloc(sizeof(LexDescr) * (size + 1)));
  /* RDS end */

  for (int i = 1; i <= size; i++) {
    descr[i - 1].lexid = i;
    descr[i - 1].alias = pstrdup(tok_alias[i]);
    descr[i - 1].descr = pstrdup(lex_descr[i]);
  }

  descr[size].lexid = 0;

  PG_RETURN_POINTER(descr);
}

Datum jieba_load_user_dict(PG_FUNCTION_ARGS)
{
  char   command[150];
  mode_t mode1;
  mode_t mode2;
  int    ret      = 0;
  int    proc     = 0;
  int    dict_num = PG_GETARG_INT32(0);
  FILE*  fp       = NULL;

  const char *tsearch_pathname = jieba_get_user_tsearch_pathname();
  const char *dict_pathname    = jieba_get_tsearch_config_filename(USER_DICT, EXT, dict_num);

  if (!jieba_status)
    elog(ERROR, "To use a user dict, pg_jieba must be loaded via shared_preload_libraries.");

  LWLockAcquire(jieba_status->lock, LW_EXCLUSIVE);

  if ((access(tsearch_pathname, F_OK)) != 0)
  {
    mode1 = S_IRUSR | S_IWUSR | S_IXUSR;
    mkdir(tsearch_pathname, mode1);
  }

  if ((access(dict_pathname, F_OK)) == 0)
  {
    remove(dict_pathname);
  }

  mode2 = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
  creat(dict_pathname, mode2);

  snprintf(command, sizeof(command), "select word,weight,type from jieba_user_dict where dict_name=%d", dict_num);
  SPI_connect();
  ret  = SPI_exec(command, 0);
  proc = SPI_processed;

  fp = fopen(dict_pathname, "w");
  
  if (ret > 0 && SPI_tuptable != NULL)
  {
    StringInfoData buf;
    SPITupleTable  *tupletable = SPI_tuptable;
    TupleDesc      tupdesc     = SPI_tuptable->tupdesc;

    for (int i = 0; i < proc; i++)
    {
      initStringInfo(&buf);
      HeapTuple tuple = tupletable->vals[i];
      for (int j = 1; j <= tupdesc->natts; j++)
      {
        appendStringInfo(&buf, "%s%s",
                         SPI_getvalue(tuple, tupdesc, j),
                         (j == tupdesc->natts) ? "\n" : " ");
      }
      fwrite(buf.data, buf.len, 1, fp);
    }
  }

  SPI_finish();
  fclose(fp);

  if (jieba != nullptr)
  {
    delete jieba;
    jieba = new PgJieba(jieba_get_tsearch_config_filename(DICT_PATH, EXT, -1),
                        jieba_get_tsearch_config_filename(HMM_PATH, EXT, -1),
                        dict_pathname);
  }
  LWLockRelease(jieba_status->lock);
  PG_RETURN_VOID();
}

static char *jieba_get_user_tsearch_pathname()
{
  char* pathname = static_cast<char*>(palloc(MAXPGPATH));
  snprintf(pathname, MAXPGPATH, "%s/tsearch_data/", DataDir);
  return pathname;
}

/*
 * Given the base name and extension of a tsearch config file, return
 * its full path name.  The base name is assumed to be user-supplied,
 * and is checked to prevent pathname attacks.  The extension is assumed
 * to be safe.
 *
 * The result is a palloc'd string.
 * 
 * RDS: add parameter num.It takes effect when it is not equal to -1, 
 * and returns the user-defined dictionary path of the corresponding number.
 */
static char* jieba_get_tsearch_config_filename(const char *basename,
                                               const char *extension,
                                               int num)
{
  char sharepath[MAXPGPATH];

  /*
   * We limit the basename to contain a-z, 0-9, and underscores.  This may
   * be overly restrictive, but we don't want to allow access to anything
   * outside the tsearch_data directory, so for instance '/' *must* be
   * rejected, and on some platforms '\' and ':' are risky as well. Allowing
   * uppercase might result in incompatible behavior between case-sensitive
   * and case-insensitive filesystems, and non-ASCII characters create other
   * interesting risks, so on the whole a tight policy seems best.
   */
  if (strspn(basename, "abcdefghijklmnopqrstuvwxyz0123456789_.") != strlen(basename))
    ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("invalid text search configuration file name \"%s\"",
            basename)));

  get_share_path(my_exec_path, sharepath);
  char* result = static_cast<char*>(palloc(MAXPGPATH));
  if (num == -1)
  {
    snprintf(result, MAXPGPATH, "%s/tsearch_data/%s.%s",
             sharepath, basename, extension);
  }
  else
  {
    char dict_num[20];
    sprintf(dict_num, "%d", num);
    snprintf(result, MAXPGPATH, "%s/tsearch_data/%s%s.%s",
             DataDir, basename, dict_num, extension);
  }
  return result;
}

/*
 * RDS:Count the number of utf8 characters in each segment, which is 
 * used to calculate the offset of each segment from the beginning of the string.
 */
static int jieba_get_utf8_char_number(const char *s)
{
  int i = 0, ret = 0;
  while (s[i])
  {
    /*
     * In UTF-8, all bytes that begin with the bit pattern 10(0x0c)
     * are subsequent bytes of a multi-byte sequence
     * 
     *                     UTF-8
     * Range              Encoding  Binary value
     * -----------------  --------  --------------------------
     * U+000000-U+00007f  0xxxxxxx  0xxxxxxx
     *
     * U+000080-U+0007ff  110yyyxx  00000yyy xxxxxxxx
     *                    10xxxxxx
     * 
     * U+000800-U+00ffff  1110yyyy  yyyyyyyy xxxxxxxx
     *                    10yyyyxx
     *                    10xxxxxx
     *
     * U+010000-U+10ffff  11110zzz  000zzzzz yyyyyyyy xxxxxxxx
     *                    10zzyyyy
     *                    10yyyyxx
     *                    10xxxxxx
     */
    if ((s[i] & 0xc0) != 0x80)
    {
      ret++;
    }
    i++;
  }
  return ret;
}

static void jieba_shmem_startup(void)
{
  bool found;

  if (prev_shmem_startup_hook)
    prev_shmem_startup_hook();

  jieba_status = NULL;
  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
  jieba_status = (jiebaSharedState *)ShmemInitStruct("jieba_shared_mem",
                                                     sizeof(jiebaSharedState),
                                                     &found);

  if (!found)
  {
    /* First time through ... */
    jieba_status->lock = &(GetNamedLWLockTranche("jieba"))->lock;
  }
  LWLockRelease(AddinShmemInitLock);
  if (found)
    return;
}

}; // namespace pg_jieba