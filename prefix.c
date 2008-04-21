/**
 * Prefix opclass allows to efficiently index a prefix table with
 * GiST.
 *
 * More common use case is telephony prefix searching for cost or
 * routing.
 *
 * Many thanks to AndrewSN, who provided great amount of help in the
 * writting of this opclass, on the PostgreSQL internals, GiST inner
 * working and prefix search analyses.
 *
 * $Id: prefix.c,v 1.38 2008/04/21 09:59:25 dim Exp $
 */

#include <stdio.h>
#include "postgres.h"

#include "access/gist.h"
#include "access/skey.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/builtins.h"
#include <math.h>

#define  DEBUG
/**
 * We use those DEBUG defines in the code, uncomment them to get very
 * verbose output.
 *
#define  DEBUG_UNION
#define  DEBUG_PENALTY
#define  DEBUG_PICKSPLIT
#define  DEBUG_PRESORT_GP
#define  DEBUG_PRESORT_MAX
#define  DEBUG_PRESORT_UNIONS
#define  DEBUG_PRESORT_RESULT

#define  DEBUG_PR_IN
#define  DEBUG_PR_NORMALIZE
#define  DEBUG_MAKE_VARLENA
*/

PG_MODULE_MAGIC;

/**
 * This code has only been tested with PostgreSQL 8.2 and 8.3
 */
#if PG_VERSION_NUM / 100 != 802 && PG_VERSION_NUM / 100 != 803
#error "Unknown or unsupported postgresql version"
#endif

/**
 * Define our own varlena size macro depending on PGVER
 */
#if PG_VERSION_NUM / 100 == 802
#define PREFIX_VARSIZE(x)        (VARSIZE(x) - VARHDRSZ)
#define PREFIX_VARDATA(x)        (VARDATA(x))
#define PREFIX_PG_GETARG_TEXT(x) (PG_GETARG_TEXT_P(x))
#define PREFIX_SET_VARSIZE(p, s) (VARATT_SIZEP(p) = s)

#else
#define PREFIX_VARSIZE(x)        (VARSIZE_ANY_EXHDR(x))
#define PREFIX_VARDATA(x)        (VARDATA_ANY(x))
#define PREFIX_PG_GETARG_TEXT(x) (PG_GETARG_TEXT_PP(x))
#define PREFIX_SET_VARSIZE(p, s) (SET_VARSIZE(p, s))
#endif

/**
 * prefix_range datatype, varlena structure
 */
typedef struct {
  char first;
  char last;
  char prefix[1]; /* this is a varlena structure, data follows */
} prefix_range;

enum pr_delimiters_t {
  PR_OPEN   = '[',
  PR_CLOSE  = ']',
  PR_SEP    = '-'
} pr_delimiters;

/**
 * prefix_range input/output functions and operators
 */
Datum prefix_range_in(PG_FUNCTION_ARGS);
Datum prefix_range_out(PG_FUNCTION_ARGS);
/*
Datum prefix_range_recv(PG_FUNCTION_ARGS);
Datum prefix_range_send(PG_FUNCTION_ARGS);
*/
Datum prefix_range_cast_to_text(PG_FUNCTION_ARGS);
Datum prefix_range_cast_from_text(PG_FUNCTION_ARGS);

Datum prefix_range_eq(PG_FUNCTION_ARGS);
Datum prefix_range_neq(PG_FUNCTION_ARGS);
Datum prefix_range_lt(PG_FUNCTION_ARGS);
Datum prefix_range_le(PG_FUNCTION_ARGS);
Datum prefix_range_gt(PG_FUNCTION_ARGS);
Datum prefix_range_ge(PG_FUNCTION_ARGS);
Datum prefix_range_cmp(PG_FUNCTION_ARGS);

Datum prefix_range_overlaps(PG_FUNCTION_ARGS);
Datum prefix_range_contains(PG_FUNCTION_ARGS);
Datum prefix_range_contains_strict(PG_FUNCTION_ARGS);
Datum prefix_range_contained_by(PG_FUNCTION_ARGS);
Datum prefix_range_contained_by_strict(PG_FUNCTION_ARGS);
Datum prefix_range_union(PG_FUNCTION_ARGS);
Datum prefix_range_inter(PG_FUNCTION_ARGS);

#define DatumGetPrefixRange(X)	          ((prefix_range *) PREFIX_VARDATA(DatumGetPointer(X)) )
#define PrefixRangeGetDatum(X)	          PointerGetDatum(make_varlena(X))
#define PG_GETARG_PREFIX_RANGE_P(n)	  DatumGetPrefixRange(PG_DETOAST_DATUM(PG_GETARG_DATUM(n)))
#define PG_RETURN_PREFIX_RANGE_P(x)	  return PrefixRangeGetDatum(x)

/**
 * Used by prefix_contains_internal and pr_contains_prefix.
 *
 * plen is the length of string p, qlen the length of string q, the
 * caller are dealing with either text * or char * and its their
 * responsabolity to use either strlen() or PREFIX_VARSIZE()
 */
static inline
bool __prefix_contains(char *p, char *q, int plen, int qlen) {
  if(qlen < plen )
    return false;

  return memcmp(p, q, plen) == 0;
}

static inline
char *__greater_prefix(char *a, char *b, int alen, int blen)
{
  int i = 0;
  char *result = NULL;

  for(i=0; i<alen && i<blen && a[i] == b[i]; i++);
  
  /* i is the last common char position in a, or 0 */
  if( i == 0 ) {
    /**
     * return ""
     */
    result = (char *)palloc(sizeof(char));
  }
  else {
    result = (char *)palloc((i+1) * sizeof(char));
    memcpy(result, a, i);
  }
  result[i] = 0;
    
  return result;
}

/**
 * First, the input reader. A prefix range will have to respect the
 * following regular expression: .*([[].-.[]])? 
 *
 * examples : 123[4-6], [1-3], 234, 01[] --- last one not covered by
 * regexp.
 */

static inline
prefix_range *build_pr(char *prefix) {
  int s = strlen(prefix) + 1;
  prefix_range *pr = palloc(sizeof(prefix_range) + s);
  memcpy(pr->prefix, prefix, s);
  pr->first = 0;
  pr->last  = 0;

#ifdef DEBUG_PR_IN
  elog(NOTICE,
       "build_pr: pr->prefix = '%s', pr->first = %d, pr->last = %d", 
       pr->prefix, pr->first, pr->last);
#endif

  return pr;
}

static inline
prefix_range *pr_normalize(prefix_range *a) {
  char tmpswap;
  char *prefix;

  prefix_range *pr = build_pr(a->prefix);
  pr->first = a->first;
  pr->last  = a->last;

  if( pr->first == pr->last ) {
    int s = strlen(pr->prefix)+2;
    prefix = (char *)palloc(s);
    memcpy(prefix, pr->prefix, s-2);
    prefix[s-2] = pr->first;
    prefix[s-1] = 0;

#ifdef DEBUG_PR_NORMALIZE
    elog(NOTICE, "prefix_range %s %s %s", str, pr->prefix, prefix);
#endif

    pfree(pr);    
    pr = build_pr(prefix);
  }
  else if( pr->first > pr->last ) {
    tmpswap   = pr->first;
    pr->first = pr->last;
    pr->last  = tmpswap;
  }
  return pr;
}

static inline
prefix_range *pr_from_str(char *str) {
  prefix_range *pr = NULL;
  char *prefix = (char *)palloc(strlen(str)+1);
  char current = 0, previous = 0;
  bool opened = false;
  bool closed = false;
  bool sawsep = false;
  char *ptr, *prefix_ptr = prefix;

  bzero(prefix, strlen(str)+1);

  for(ptr=str; *ptr != 0; ptr++) {
    previous = current;
    current = *ptr;

    if( !opened && current != PR_OPEN )
      *prefix_ptr++ = current;

#ifdef DEBUG_PR_IN
    elog(NOTICE, "prefix_range previous='%c' current='%c' prefix='%s'", 
	 (previous?previous:' '), current, prefix);
#endif

    switch( current ) {

    case PR_OPEN:
      if( opened ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s contains several %c", str, PR_OPEN);
#endif
	return NULL;
      }
      opened = true;

      pr = build_pr(prefix);
      break;

    case PR_SEP:
      if( opened ) {
	if( closed ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s contains trailing character", str);
#endif
	  return NULL;
	}
	sawsep = true;

	if( previous == PR_OPEN ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s has separator following range opening, without data", str);
#endif	  
	  return NULL;
	}

	pr->first = previous;
      }
      break;

    case PR_CLOSE:
      if( !opened ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s closes a range which is not opened ", str);
#endif
	return NULL;
      }

      if( closed ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s contains several %c", str, PR_CLOSE);
#endif
	return NULL;
      }
      closed = true;

      if( sawsep ) {
	if( previous == PR_SEP ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s has a closed range without last bound", str);	  
#endif
	  return NULL;
	}
	pr->last = previous;
      }
      else {
	if( previous != PR_OPEN ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s has a closing range without separator", str);
#endif
	  return NULL;
	}
      }
      break;

    default:
      if( closed ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s contains trailing characters", str);
#endif
	return NULL;
      } 
      break;
    }
  }

  if( ! opened ) {
    pr = build_pr(prefix);
  }

  if( opened && !closed ) {
#ifdef DEBUG_PR_IN
    elog(ERROR, "prefix_range %s opens a range but does not close it", str);
#endif
    return NULL;
  }

  pr = pr_normalize(pr);

#ifdef DEBUG_PR_IN
  if( pr != NULL ) {
    if( pr->first && pr->last )
      elog(NOTICE,
	   "prefix_range %s: prefix = '%s', first = '%c', last = '%c'", 
	   str, pr->prefix, pr->first, pr->last);
    else
      elog(NOTICE,
	   "prefix_range %s: prefix = '%s', no first nor last", 
	   str, pr->prefix);
  }
#endif

  return pr;
}

static inline
struct varlena *make_varlena(prefix_range *pr) {
  struct varlena *vdat;
  int size;
  
  if (pr != NULL) {
    size = sizeof(prefix_range) + ((strlen(pr->prefix)+1)*sizeof(char)) + VARHDRSZ;
    vdat = palloc(size);
    PREFIX_SET_VARSIZE(vdat, size);
    memcpy(VARDATA(vdat), pr, (size - VARHDRSZ));

#ifdef DEBUG_MAKE_VARLENA
    elog(NOTICE, "make varlena: size=%d varsize=%d compressed=%c external=%c %s[%c-%c] %s[%c-%c]",
	 size,
	 PREFIX_VARSIZE(vdat),
	 (VARATT_IS_COMPRESSED(vdat) ? 't' : 'f'),
	 (VARATT_IS_EXTERNAL(vdat)   ? 't' : 'f'),
	 pr->prefix,
	 (pr->first != 0 ? pr->first : ' '),
	 (pr->last  != 0 ? pr->last  : ' '),
	 ((prefix_range *)VARDATA(vdat))->prefix,
	 (((prefix_range *)VARDATA(vdat))->first ? ((prefix_range *)VARDATA(vdat))->first : ' '),
	 (((prefix_range *)VARDATA(vdat))->last  ? ((prefix_range *)VARDATA(vdat))->last  : ' '));
#endif

    return vdat;
  }
  return NULL;
}

static inline
bool pr_eq(prefix_range *a, prefix_range *b) {
  int sa = strlen(a->prefix);
  int sb = strlen(b->prefix);

  return sa == sb
    && memcmp(a->prefix, b->prefix, sa) == 0
    && a->first == b->first 
    && a->last  == b->last;
}

static inline
bool pr_lt(prefix_range *a, prefix_range *b, bool eqval) {
  int cmp = 0;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  int mlen = alen;
  char *p  = a->prefix;
  char *q  = b->prefix;

  if( alen == blen ) {
    cmp = memcmp(p, q, alen);

    if( cmp < 0 ) 
      return true;

    else if( cmp == 0 ) {
      if( a->first == 0 ) {
	if( b->first == 0 )
	  return eqval;
	return true;
      }
      else	
	return (eqval ? a->first <= b->first : a->first < b->first);
    }
    else
      return false;
  }
  if( mlen > blen )
    mlen = blen;

  if( alen == 0 && a->first != 0 ) {
    return (eqval ? (a->first <= q[0]) : (a->first < q[0]));
  }
  else if( blen == 0 && b->first != 0 ) {
    return (eqval ? (p[0] <= b->first) : (p[0] < b->first));
  }
  else
    return (eqval ? memcmp(p, q, mlen) <= 0 : memcmp(p, q, mlen) < 0);
}

static inline
bool pr_gt(prefix_range *a, prefix_range *b, bool eqval) {
  int cmp = 0;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  int mlen = alen;
  char *p  = a->prefix;
  char *q  = b->prefix;

  if( alen == blen ) {
    cmp = memcmp(p, q, alen);

    if( cmp > 0 )
      return true;

    else if( cmp == 0 ) {
      if( a->last == 0 ) {
	if( b->last == 0 )
	  return eqval;
	return false;
      }
      else	
	return (eqval ? a->last >= b->last : a->last > b->last);
    }
    else
      return false;
  }
  if( mlen > blen )
    mlen = blen;

  if( alen == 0 && a->last != 0 ) {
    return (eqval ? (a->last >= q[0]) : (a->last > q[0]));
  }
  else if( blen == 0 && b->first != 0 ) {
    return (eqval ? (p[0] >= b->last) : (p[0] > b->last));
  }
  else
    return (eqval ? memcmp(p, q, mlen) >= 0 : memcmp(p, q, mlen) > 0);
}

static inline
int pr_cmp(prefix_range *a, prefix_range *b) {
  if( pr_eq(a, b) )
    return 0;

  if( pr_lt(a, b, false) )
    return -1;

  return 1;
}

static inline
bool pr_contains(prefix_range *left, prefix_range *right, bool eqval) {
  int sl;
  int sr;
  bool left_prefixes_right;

  if( pr_eq(left, right) )
    return eqval;

  sl = strlen(left->prefix);
  sr = strlen(right->prefix);

  if( sr < sl )
    return false;

  left_prefixes_right = memcmp(left->prefix, right->prefix, sl) == 0;

  if( left_prefixes_right ) {
    if( sl == sr )
      return left->first == 0 ||
	(left->first <= right->first && left->last >= right->last);

    return left->first == 0 ||
      (left->first <= right->prefix[sl] && right->prefix[sl] <= left->last);
  }
  return false;
}

/**
 * does a given prefix_range includes a given prefix?
 */
static inline
bool pr_contains_prefix(prefix_range *pr, text *query, bool eqval) {
  int plen = strlen(pr->prefix);
  int qlen = PREFIX_VARSIZE(query);
  char *p  = pr->prefix;
  char *q  = (char *)PREFIX_VARDATA(query);

  if( __prefix_contains(p, q, plen, qlen) ) {
    if( pr->first == 0 || qlen == plen ) {
      return eqval;
    }

    /**
     * __prefix_contains() is true means qlen >= plen, and previous
     * test ensures qlen != plen, we hence assume qlen > plen.
     */
    Assert(qlen > plen);
    return pr-> first <= q[plen] && q[plen] <= pr->last;
  }
  return false;
}

static inline
prefix_range *pr_union(prefix_range *a, prefix_range *b) {
  prefix_range *res = NULL;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  char *gp = NULL;
  int gplen;
  char min, max;

  if( 0 == alen && 0 == blen ) {
    res = build_pr("");
    res->first = a->first <= b->first ? a->first : b->first;
    res->last  = a->last  >= b->last  ? a->last : b->last;
    return pr_normalize(res);
  }

  gp = __greater_prefix(a->prefix, b->prefix, alen, blen);
  gplen = strlen(gp);

  if( gplen == 0 ) {
    res = build_pr("");
    if( alen > 0 && blen > 0 ) {
      res->first = a->prefix[0];
      res->last  = b->prefix[0];
    }
    else if( alen == 0 ) {
      res->first = a->first <= b->prefix[0] ? a->first : b->prefix[0];
      res->last  = a->last  >= b->prefix[0] ? a->last  : b->prefix[0];
    }
    else if( blen == 0 ) {
      res->first = b->first <= a->prefix[0] ? b->first : a->prefix[0];
      res->last  = b->last  >= a->prefix[0] ? b->last  : a->prefix[0];
    }
  }
  else {
    res = build_pr(gp);

    if( gplen == alen && alen == blen ) {
      res->first = a->first <= b->first ? a->first : b->first;
      res->last  = a->last  >= b->last  ? a->last : b->last;
    }
    else if( gplen == alen ) {
      Assert(alen < blen);
      res->first = a->first <= b->prefix[alen] ? a->first : b->prefix[alen];
      res->last  = a->last  >= b->prefix[alen] ? a->last  : b->prefix[alen];
    }
    else if( gplen == blen ) {
      Assert(blen < alen);
      res->first = b->first <= a->prefix[blen] ? b->first : a->prefix[blen];
      res->last  = b->last  >= a->prefix[blen] ? b->last  : a->prefix[blen];
    }
    else {
      Assert(gplen < alen && gplen < blen);
      min = a->prefix[gplen];
      max = b->prefix[gplen];

      if( min > max ) {
	min = b->prefix[gplen];
	max = a->prefix[gplen];
      }
      res->first = min;
      res->last  = max;
    }
  }
  return pr_normalize(res);
}

static inline
prefix_range *pr_inter(prefix_range *a, prefix_range *b) {
  prefix_range *res = NULL;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  char *gp = NULL;
  int gplen;

  if( 0 == alen && 0 == blen ) {
    res = build_pr("");
    res->first = a->first > b->first ? a->first : b->first;
    res->last  = a->last  < b->last  ? a->last  : b->last;
    return pr_normalize(res);
  }

  gp = __greater_prefix(a->prefix, b->prefix, alen, blen);
  gplen = strlen(gp);

  if( gplen != alen && gplen != blen ) {
    return build_pr("");
  }

  if( gplen == alen && 0 == alen ) {
    if( a->first <= b->prefix[0] && b->prefix[0] <= a->last ) {      
      res = build_pr(b->prefix);
      res->first = b->first;
      res->last  = b->last;
    }
    else
      res = build_pr("");
  }
  else if( gplen == blen && 0 == blen ) {
    if( b->first <= a->prefix[0] && a->prefix[0] <= b->last ) {      
      res = build_pr(a->prefix);
      res->first = a->first;
      res->last  = a->last;
    }
    else
      res = build_pr("");
  }
  else if( gplen == alen && alen == blen ) {
    res = build_pr(gp);
    res->first = a->first > b->first ? a->first : b->first;
    res->last  = a->last  < b->last  ? a->last  : b->last;
  }
  else if( gplen == alen ) {
    Assert(gplen < blen);
    res = build_pr(b->prefix);
    res->first = b->first;
    res->last  = b->last;
  }
  else if( gplen == blen ) {
    Assert(gplen < alen);
    res = build_pr(a->prefix);
    res->first = a->first;
    res->last  = a->last;
  }

  return pr_normalize(res);
}

/**
 * true if ranges have at least one common element
 */
static inline
bool pr_overlaps(prefix_range *a, prefix_range *b) {
  prefix_range *inter = pr_inter(a, b);

  return strlen(inter->prefix) > 0 || (inter->first != 0 && inter->last != 0);
}


PG_FUNCTION_INFO_V1(prefix_range_in);
Datum
prefix_range_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);
    prefix_range *pr = pr_from_str(str);

    if (pr != NULL) {
      PG_RETURN_PREFIX_RANGE_P(pr);
    }

    ereport(ERROR,
	    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	     errmsg("invalid prefix_range value: \"%s\"", str)));
    PG_RETURN_NULL();
}


PG_FUNCTION_INFO_V1(prefix_range_out);
Datum
prefix_range_out(PG_FUNCTION_ARGS)
{
  prefix_range *pr = PG_GETARG_PREFIX_RANGE_P(0);
  char *out = NULL;

  if( pr->first ) {
    out = (char *)palloc((strlen(pr->prefix)+6) * sizeof(char));
    sprintf(out, "%s[%c-%c]", pr->prefix, pr->first, pr->last);
  }
  else {
    out = (char *)palloc((strlen(pr->prefix)+3) * sizeof(char));
    sprintf(out, "%s[]", pr->prefix);
  }
  PG_RETURN_CSTRING(out);
}

PG_FUNCTION_INFO_V1(prefix_range_cast_from_text);
Datum
prefix_range_cast_from_text(PG_FUNCTION_ARGS)
{
  text *txt = PG_GETARG_TEXT_P(0);
  Datum cstring = DirectFunctionCall1(textout, PointerGetDatum(txt));
  return DirectFunctionCall1(prefix_range_in, cstring);
}

PG_FUNCTION_INFO_V1(prefix_range_cast_to_text);
Datum
prefix_range_cast_to_text(PG_FUNCTION_ARGS)
{
  prefix_range *pr = PG_GETARG_PREFIX_RANGE_P(0);
  Datum cstring;
  text *out;

  if (pr != NULL) {
    cstring = DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(pr));
    out = (text *)DirectFunctionCall1(textin, cstring);

    PG_RETURN_TEXT_P(out);
  }
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(prefix_range_eq);
Datum
prefix_range_eq(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_eq(PG_GETARG_PREFIX_RANGE_P(0), 
			PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_neq);
Datum
prefix_range_neq(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( ! pr_eq(PG_GETARG_PREFIX_RANGE_P(0), 
			  PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_lt);
Datum
prefix_range_lt(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_lt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			FALSE) );
}

PG_FUNCTION_INFO_V1(prefix_range_le);
Datum
prefix_range_le(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_lt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			TRUE) );
}

PG_FUNCTION_INFO_V1(prefix_range_gt);
Datum
prefix_range_gt(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_gt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			FALSE) );
}

PG_FUNCTION_INFO_V1(prefix_range_ge);
Datum
prefix_range_ge(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_gt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			TRUE) );
}

PG_FUNCTION_INFO_V1(prefix_range_cmp);
Datum
prefix_range_cmp(PG_FUNCTION_ARGS)
{
  prefix_range *a = PG_GETARG_PREFIX_RANGE_P(0);
  prefix_range *b = PG_GETARG_PREFIX_RANGE_P(1);

  PG_RETURN_INT32(pr_cmp(a, b));
}

PG_FUNCTION_INFO_V1(prefix_range_overlaps);
Datum
prefix_range_overlaps(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_overlaps(PG_GETARG_PREFIX_RANGE_P(0), 
			      PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_contains);
Datum
prefix_range_contains(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(0), 
			      PG_GETARG_PREFIX_RANGE_P(1),
			      TRUE ));
}

PG_FUNCTION_INFO_V1(prefix_range_contains_strict);
Datum
prefix_range_contains_strict(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(0),
			      PG_GETARG_PREFIX_RANGE_P(1),
			      FALSE ));
}

PG_FUNCTION_INFO_V1(prefix_range_contained_by);
Datum
prefix_range_contained_by(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(1),
			      PG_GETARG_PREFIX_RANGE_P(0),
			      TRUE ));
}

PG_FUNCTION_INFO_V1(prefix_range_contained_by_strict);
Datum
prefix_range_contained_by_strict(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(1),
			      PG_GETARG_PREFIX_RANGE_P(0),
			      FALSE ));
}

PG_FUNCTION_INFO_V1(prefix_range_union);
Datum
prefix_range_union(PG_FUNCTION_ARGS)
{
  PG_RETURN_PREFIX_RANGE_P( pr_union(PG_GETARG_PREFIX_RANGE_P(0),
				     PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_inter);
Datum
prefix_range_inter(PG_FUNCTION_ARGS)
{
  PG_RETURN_PREFIX_RANGE_P( pr_inter(PG_GETARG_PREFIX_RANGE_P(0),
				     PG_GETARG_PREFIX_RANGE_P(1)) );
}

/**
 * GiST support methods
 *
 * pr_penalty allows SQL level penalty code testing.
 */
Datum gpr_consistent(PG_FUNCTION_ARGS);
Datum gpr_compress(PG_FUNCTION_ARGS);
Datum gpr_decompress(PG_FUNCTION_ARGS);
Datum gpr_penalty(PG_FUNCTION_ARGS);
Datum gpr_picksplit(PG_FUNCTION_ARGS);
Datum gpr_picksplit_jordan(PG_FUNCTION_ARGS);
Datum gpr_union(PG_FUNCTION_ARGS);
Datum gpr_same(PG_FUNCTION_ARGS);
Datum pr_penalty(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gpr_consistent);
Datum
gpr_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    prefix_range *query = PG_GETARG_PREFIX_RANGE_P(1);
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    prefix_range *key = DatumGetPrefixRange(entry->key);
    bool retval;

    /**
     * We only have 1 Strategy (operator @>)
     * and we want to avoid compiler complaints that we do not use it.
     */ 
    Assert(strategy == 1);
    (void) strategy;
    retval = pr_contains(key, query, true);

    PG_RETURN_BOOL(retval);
}

/*
 * GiST Compress and Decompress methods for prefix_range
 * do not do anything.
 */
PG_FUNCTION_INFO_V1(gpr_compress);
Datum
gpr_compress(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(gpr_decompress);
Datum
gpr_decompress(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

static inline
float __pr_penalty(prefix_range *orig, prefix_range *new)
{
  float penalty;
  char *gp;
  int  nlen, olen, gplen, dist = 0;
  char tmp;

  if( orig->prefix[0] != 0 ) {
    /**
     * The prefix main test case deals with phone number data, hence
     * containing only numbers...
     */
    if( orig->prefix[0] < '0' || orig->prefix[0] > '9' )
#ifdef DEBUG
      elog(NOTICE, "__pr_penalty(%s, %s) orig->first=%d orig->last=%d ", 
	   DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(orig))),
	   DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(new))),
	   orig->first, orig->last);
#endif
    Assert(orig->prefix[0] >= '0' && orig->prefix[0] <= '9');
  }

  olen  = strlen(orig->prefix);
  nlen  = strlen(new->prefix);
  gp    = __greater_prefix(orig->prefix, new->prefix, olen, nlen);
  gplen = strlen(gp);

  dist  = 1;

  if( 0 == olen && 0 == nlen ) {
    if( orig->last >= new->first )
      dist = 0;
    else
      dist = new->first - orig->last;
  }
  else if( 0 == olen ) {
    /**
     * penalty('[a-b]', 'xyz');
     */
    if( orig->first != 0 ) {
      tmp = new->prefix[0];

      if( orig->first <= tmp && tmp <= orig->last ) {
	gplen = 1;

	dist = 1 + (int)tmp - (int)orig->first;
	if( (1 + (int)orig->last - (int)tmp) < dist )
	  dist = 1 + (int)orig->last - (int)tmp;
      }
      else
	dist = (orig->first > tmp ? orig->first - tmp  : tmp - orig->last );
    }
  }
  else if( 0 == nlen ) {
    /**
     * penalty('abc', '[x-y]');
     */
    if( new->first != 0 ) {
      tmp = orig->prefix[0];

      if( new->first <= tmp && tmp <= new->last ) {
	gplen = 1;

	dist = 1 + (int)tmp - (int)new->first;
	if( (1 + (int)new->last - (int)tmp) < dist )
	  dist = 1 + (int)new->last - (int)tmp;
      }
      else
	dist = (new->first > tmp ? new->first - tmp  : tmp - new->last );
    }
  }
  else {
    /**
     * General case
     */

    if( gplen > 0 ) {
      if( olen > gplen && nlen == gplen && new->first != 0 ) {
	/**
	 * gpr_penalty('abc[f-l]', 'ab[x-y]')
	 */
	if( new->first <= orig->prefix[gplen]
	    && orig->prefix[gplen] <= new->last ) {

	  dist   = 1 + (int)orig->prefix[gplen] - (int)new->first;	  
	  if( (1 + (int)new->last - (int)orig->prefix[gplen]) < dist )
	    dist = 1 + (int)new->last - (int)orig->prefix[gplen];

	  gplen += 1;
	}
	else {
	  dist += 1;
	}
      }
      else if( nlen > gplen && olen == gplen && orig->first != 0 ) {
	/**
	 * gpr_penalty('ab[f-l]', 'abc[x-y]')
	 */
	if( orig->first <= new->prefix[gplen]
	    && new->prefix[gplen] <= orig->last ) {

	  dist   = 1 + (int)new->prefix[gplen] - (int)orig->first;
	  if( (1 + (int)orig->last - (int)new->prefix[gplen]) < dist )
	    dist = 1 + (int)orig->last - (int)new->prefix[gplen];

	  gplen += 1;
	}
	else {
	  dist += 1;
	}
      }	
    }
    /**
     * penalty('abc[f-l]', 'xyz[g-m]'), nothing common
     * dist = 1, gplen = 0, penalty = 1
     */
  }
  penalty = (((float)dist) / powf(256, gplen));

#ifdef DEBUG_PENALTY
  elog(NOTICE, "__pr_penalty(%s, %s) == %d/(256^%d) == %g", 
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(orig))),
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(new))),
       dist, gplen, penalty);
#endif

  return penalty;
}

PG_FUNCTION_INFO_V1(gpr_penalty);
Datum
gpr_penalty(PG_FUNCTION_ARGS)
{
  GISTENTRY *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
  GISTENTRY *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
  float *penalty = (float *) PG_GETARG_POINTER(2);
  
  prefix_range *orig = DatumGetPrefixRange(origentry->key);
  prefix_range *new  = DatumGetPrefixRange(newentry->key);

  *penalty = __pr_penalty(orig, new);
  PG_RETURN_POINTER(penalty);
}

PG_FUNCTION_INFO_V1(pr_penalty);
Datum
pr_penalty(PG_FUNCTION_ARGS)
{
  float penalty = __pr_penalty(PG_GETARG_PREFIX_RANGE_P(0),
			       PG_GETARG_PREFIX_RANGE_P(1));
  PG_RETURN_FLOAT4(penalty);
}

static int gpr_cmp(const GISTENTRY **e1, const GISTENTRY **e2) {
  prefix_range *k1 = DatumGetPrefixRange((*e1)->key);
  prefix_range *k2 = DatumGetPrefixRange((*e2)->key);

  return pr_cmp(k1, k2);
}

/**
 * Median idea from Jordan:
 *
 * sort the entries and choose a cut point near the median, being
 * careful not to cut a group sharing a common prefix when sensible.
 */
PG_FUNCTION_INFO_V1(gpr_picksplit_jordan);
Datum
gpr_picksplit_jordan(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    OffsetNumber maxoff = entryvec->n - 1;
    GISTENTRY *ent      = entryvec->vector;
    GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

    int	i, nbytes;
    OffsetNumber *left, *right;
    prefix_range *tmp_union;
    prefix_range *unionL;
    prefix_range *unionR;

    GISTENTRY **raw_entryvec;
    int cut, cut_tolerance, lower_dist, upper_dist;

    maxoff = entryvec->n - 1;
    nbytes = (maxoff + 1) * sizeof(OffsetNumber);

    v->spl_left  = (OffsetNumber *) palloc(nbytes);
    left         = v->spl_left;
    v->spl_nleft = 0;

    v->spl_right  = (OffsetNumber *) palloc(nbytes);
    right         = v->spl_right;
    v->spl_nright = 0;

    unionL = NULL;
    unionR = NULL;

    /* Initialize the raw entry vector. */
    raw_entryvec = (GISTENTRY **) malloc(entryvec->n * sizeof(void *));
    for (i=FirstOffsetNumber; i <= maxoff; i=OffsetNumberNext(i))
      raw_entryvec[i] = &(entryvec->vector[i]);
    
    /* Sort the raw entry vector. */
    pg_qsort(&raw_entryvec[1], maxoff, sizeof(void *), &gpr_cmp);

    /* 
     * Find the distance between the middle of the raw entry vector and the
     * lower-index of the first group.
     */
    cut = maxoff / 2;
    cut_tolerance = cut / 2;
    for (i=cut - 1; i > FirstOffsetNumber; i=OffsetNumberPrev(i)) {
      tmp_union = pr_union(DatumGetPrefixRange(ent[i].key),
			   DatumGetPrefixRange(ent[i+1].key));

      if( strlen(tmp_union->prefix) == 0 )
	break;
    }
    lower_dist = cut - i;
	
    /* 
     * Find the distance between the middle of the raw entry vector and the
     * upper-index of the first group.
     */
    for (i=1 + cut; i < maxoff; i=OffsetNumberNext(i)) {
      tmp_union = pr_union(DatumGetPrefixRange(ent[i].key),
			   DatumGetPrefixRange(ent[i-1].key));

      if( strlen(tmp_union->prefix) == 0 )
	break;
    }
    upper_dist = i - cut;

    /*
     * Choose the cut based on whichever falls within the cut tolerance and
     * is closer to the midpoint.  Choose one at random it there is a tie.
     *
     * If neither are within the tolerance, use the midpoint as the default. 
     */
    if (lower_dist <= cut_tolerance || upper_dist <= cut_tolerance) {
      if (lower_dist < upper_dist)
	cut -= lower_dist;
      else if (upper_dist < lower_dist)
	cut += upper_dist;
      else
	cut = (random() % 2) ? (cut - lower_dist) : (cut + upper_dist);
    }
	
    for (i=FirstOffsetNumber; i <= maxoff; i=OffsetNumberNext(i)) {
      int real_index = raw_entryvec[i] - entryvec->vector;
      // datum_alpha = VECGETKEY(entryvec, real_index);
      tmp_union = DatumGetPrefixRange(entryvec->vector[real_index].key);

      Assert(tmp_union != NULL);

      /* Put everything below the cut in the left node. */
      if (i < cut) {
	if( unionL == NULL )
	  unionL = tmp_union;
	else
	  unionL = pr_union(unionL, tmp_union);

	*left = real_index;
	++left;
	++(v->spl_nleft);
      }
      /* And put everything else in the right node. */
      else {
	if( unionR == NULL )
	  unionR = tmp_union;
	else
	  unionR = pr_union(unionR, tmp_union);

	*right = real_index;
	++right;
	++(v->spl_nright);
      }
    }

    *left = *right = FirstOffsetNumber; /* sentinel value, see dosplit() */
    v->spl_ldatum = PrefixRangeGetDatum(unionL);
    v->spl_rdatum = PrefixRangeGetDatum(unionR);
    PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(gpr_picksplit);
Datum
gpr_picksplit(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    OffsetNumber maxoff = entryvec->n - 1;
    GISTENTRY *ent      = entryvec->vector;
    GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

    int	nbytes;
    OffsetNumber offl, offr;
    OffsetNumber *listL;
    OffsetNumber *listR;
    prefix_range *curl, *curr, *tmp_union;
    prefix_range *unionL;
    prefix_range *unionR;
    
    /**
     * Keeping track of penalties to insert into ListL or ListR, for
     * both the leftmost and the rightmost element of the remaining
     * list.
     */
    float pll, plr, prl, prr;

    nbytes = (maxoff + 1) * sizeof(OffsetNumber);
    listL = (OffsetNumber *) palloc(nbytes);
    listR = (OffsetNumber *) palloc(nbytes);
    v->spl_left  = listL;
    v->spl_right = listR;
    v->spl_nleft = v->spl_nright = 0;

    offl = FirstOffsetNumber;
    offr = maxoff;

    unionL = DatumGetPrefixRange(ent[offl].key);
    unionR = DatumGetPrefixRange(ent[offr].key);

    v->spl_left[v->spl_nleft++]   = offl;
    v->spl_right[v->spl_nright++] = offr;
    v->spl_left  = listL;
    v->spl_right = listR;

    offl = OffsetNumberNext(offl);
    offr = OffsetNumberPrev(offr);

    while( offl < offr ) {
      curl = DatumGetPrefixRange(ent[offl].key);
      curr = DatumGetPrefixRange(ent[offr].key);

#ifdef DEBUG_PICKSPLIT
      elog(NOTICE, "gpr_picksplit: ent[%3d] = '%s' \tent[%3d] = '%s'",
	   offl,
	   DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(curl))),
	   offr,
	   DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(curr))));
#endif

      Assert(curl != NULL && curr != NULL);

      pll = __pr_penalty(unionL, curl);
      plr = __pr_penalty(unionR, curl);
      prl = __pr_penalty(unionL, curr);
      prr = __pr_penalty(unionR, curr);

      if( pll <= plr && prl >= prr ) {
	/**
	 * curl should go to left and curr to right, unless they share
	 * a non-empty common prefix, in which case we place both curr
	 * and curl on the same side. Arbitrarily the left one.
	 */
	if( pll == plr && prl == prr ) {
	  tmp_union = pr_union(curl, curr);

	  if( strlen(tmp_union->prefix) > 0 ) {
	    unionL = pr_union(unionL, tmp_union);
	    v->spl_left[v->spl_nleft++] = offl;
	    v->spl_left[v->spl_nleft++] = offr;

	    offl = OffsetNumberNext(offl);
	    offr = OffsetNumberPrev(offr);
	    continue;
	  }
	}
	/**
	 * here pll <= plr and prl >= prr and (pll != plr || prl != prr)
	 */
	unionL = pr_union(unionL, curl);
	unionR = pr_union(unionR, curr);

	v->spl_left[v->spl_nleft++]   = offl;
	v->spl_right[v->spl_nright++] = offr;

	offl = OffsetNumberNext(offl);
	offr = OffsetNumberPrev(offr);
      }
      else if( pll > plr && prl >= prr ) {
	/**
	 * Current rightmost entry is added to listL
	 */
	unionR = pr_union(unionR, curr);
	v->spl_right[v->spl_nright++] = offr;
	offr = OffsetNumberPrev(offr);
      }
      else if( pll <= plr && prl < prr ) {
	/**
	 * Current leftmost entry is added to listL
	 */
	unionL = pr_union(unionL, curl);
	v->spl_left[v->spl_nleft++] = offl;
	offl = OffsetNumberNext(offl);
      }
      else if( (pll - plr) < (prr - prl) ) {
	/**
	 * All entries still in the list go into listL
	 */
	for(; offl <= offr; offl = OffsetNumberNext(offl)) {
	  curl   = DatumGetPrefixRange(ent[offl].key);
	  unionL = pr_union(unionL, curl);
	  v->spl_left[v->spl_nleft++] = offl;
	}
      }
      else {
	/**
	 * All entries still in the list go into listR
	 */
	for(; offr >= offl; offr = OffsetNumberPrev(offr)) {
	  curr   = DatumGetPrefixRange(ent[offr].key);
	  unionR = pr_union(unionR, curr);
	  v->spl_right[v->spl_nright++] = offr;
	}
      }
    }

    /**
     * The for loop continues while offl < offr. If maxoff is odd, it
     * could be that there's a last value to process. Here we choose
     * where to add it.
     */
    if( offl == offr ) {
      curl = DatumGetPrefixRange(ent[offl].key);

      pll  = __pr_penalty(unionL, curl);
      plr  = __pr_penalty(unionR, curl);

      if( pll < plr || (pll == plr && v->spl_nleft < v->spl_nright) ) {
	curl       = DatumGetPrefixRange(ent[offl].key);
	unionL     = pr_union(unionL, curl);
	v->spl_left[v->spl_nleft++] = offl;
      }
      else {
	curl       = DatumGetPrefixRange(ent[offl].key);
	unionR     = pr_union(unionR, curl);
	v->spl_right[v->spl_nright++] = offl;
      }
    }

    v->spl_ldatum = PrefixRangeGetDatum(unionL);
    v->spl_rdatum = PrefixRangeGetDatum(unionR);

    /**
     * All read entries (maxoff) should have make it to the
     * GIST_SPLITVEC return value.
     */
    Assert(maxoff = v->spl_nleft+v->spl_nright);

#ifdef DEBUG_PICKSPLIT
    elog(NOTICE, "gpr_picksplit(): entryvec->n=%4d maxoff=%4d l=%4d r=%4d l+r=%4d unionL='%s' unionR='%s'",
	 entryvec->n, maxoff, v->spl_nleft, v->spl_nright, v->spl_nleft+v->spl_nright,
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, v->spl_ldatum)),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, v->spl_rdatum)));
#endif
	
    PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(gpr_union);
Datum
gpr_union(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    GISTENTRY *ent = entryvec->vector;

    prefix_range *out, *tmp, *old;
    int	numranges, i = 0;

    numranges = entryvec->n;
    tmp = DatumGetPrefixRange(ent[0].key);
    out = tmp;

    if( numranges == 1 ) {
      out = build_pr(tmp->prefix);
      out->first = tmp->first;
      out->last  = tmp->last;

      PG_RETURN_PREFIX_RANGE_P(out);
    }
  
    for (i = 1; i < numranges; i++) {
      old = out;
      tmp = DatumGetPrefixRange(ent[i].key);
      out = pr_union(out, tmp);

#ifdef DEBUG_UNION
    elog(NOTICE, "gpr_union: %s | %s = %s",
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(old))),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(tmp))),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(out))));
#endif
    }

    /*
#ifdef DEBUG_UNION
    elog(NOTICE, "gpr_union: %s", 
	 DatumGetCString(DirectFunctionCall1(prefix_range_out,
					     PrefixRangeGetDatum(out))));
#endif
    */
    PG_RETURN_PREFIX_RANGE_P(out);
}

PG_FUNCTION_INFO_V1(gpr_same);
Datum
gpr_same(PG_FUNCTION_ARGS)
{
    prefix_range *v1 = PG_GETARG_PREFIX_RANGE_P(0);
    prefix_range *v2 = PG_GETARG_PREFIX_RANGE_P(1);
    bool *result = (bool *) PG_GETARG_POINTER(2);

    *result = pr_eq(v1, v2);
    PG_RETURN_POINTER( result );
}




/**
 * - Operator prefix @> query and query <@ prefix
 * - greater_prefix, exposed as a func and an aggregate
 * - prefix_penalty, exposed for testing purpose
 */
Datum prefix_contains(PG_FUNCTION_ARGS);
Datum prefix_contained_by(PG_FUNCTION_ARGS);
Datum greater_prefix(PG_FUNCTION_ARGS);
Datum prefix_penalty(PG_FUNCTION_ARGS);

/**
 * GiST support methods
 */
Datum gprefix_consistent(PG_FUNCTION_ARGS);
Datum gprefix_compress(PG_FUNCTION_ARGS);
Datum gprefix_decompress(PG_FUNCTION_ARGS);
Datum gprefix_penalty(PG_FUNCTION_ARGS);
Datum gprefix_picksplit(PG_FUNCTION_ARGS);
Datum gprefix_union(PG_FUNCTION_ARGS);
Datum gprefix_same(PG_FUNCTION_ARGS);

/**
 * prefix opclass only provides 1 operator, @>
 */
static inline
bool prefix_contains_internal(text *prefix, text *query, bool eqval)
{
  int plen = PREFIX_VARSIZE(prefix);
  int qlen = PREFIX_VARSIZE(query);
  char *p  = PREFIX_VARDATA(prefix);
  char *q  = PREFIX_VARDATA(query);

  if( __prefix_contains(p, q, plen, qlen) )
    return eqval;

  return false;
}

/**
 * The operator @> code
 */
PG_FUNCTION_INFO_V1(prefix_contains);
Datum
prefix_contains(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( prefix_contains_internal(PREFIX_PG_GETARG_TEXT(0),
					   PREFIX_PG_GETARG_TEXT(1),
					   true) );
}

/**
 * The commutator, <@, using the same internal code
 */
PG_FUNCTION_INFO_V1(prefix_contained_by);
Datum
prefix_contained_by(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL( prefix_contains_internal(PREFIX_PG_GETARG_TEXT(1),
					     PREFIX_PG_GETARG_TEXT(0),
					     true) );
}

/**
 * greater_prefix returns the greater prefix of any 2 given texts
 */
static inline
text *greater_prefix_internal(text *a, text *b)
{
  int i    = 0;
  int la   = PREFIX_VARSIZE(a);
  int lb   = PREFIX_VARSIZE(b);
  char *ca = PREFIX_VARDATA(a);
  char *cb = PREFIX_VARDATA(b);

  for(i=0; i<la && i<lb && ca[i] == cb[i]; i++);
  
  /* i is the last common char position in a, or 0 */
  if( i == 0 )
    return DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum("")));
  else
    return DatumGetTextPSlice(PointerGetDatum(a), 0, i);
}

PG_FUNCTION_INFO_V1(greater_prefix);
Datum
greater_prefix(PG_FUNCTION_ARGS)
{

  PG_RETURN_POINTER( greater_prefix_internal(PREFIX_PG_GETARG_TEXT(0),
					     PREFIX_PG_GETARG_TEXT(1)) );
}

/**
 * penalty internal function, which is called in more places than just
 * gist penalty() function, namely picksplit() uses it too.
 *
 * Consider greater common prefix length, the greater the better, then
 * for a distance of 1 (only last prefix char is different), consider
 * char code distance.
 *
 * With gplen the size of the greatest common prefix and dist the char
 * code distance, the following maths should do (per AndrewSN):
 *
 * penalty() = dist / (256 ^ gplen)
 *
 * penalty(01,   03) == 2 / (256^1)
 * penalty(123, 125) == 2 / (256^2)
 * penalty(12,   56) == 4 / (256^0)
 * penalty(0, 17532) == 1 / (256^0)
 *
 * 256 is then number of codes any text position (char) can admit.
 */
static inline
float prefix_penalty_internal(text *orig, text *new)
{
  float penalty;
  text *gp;
  int  nlen, olen, gplen, dist = 0;

  olen  = PREFIX_VARSIZE(orig);
  nlen  = PREFIX_VARSIZE(new);
  gp    = greater_prefix_internal(orig, new);
  gplen = PREFIX_VARSIZE(gp);

  /**
   * greater_prefix length is orig length only if orig == gp
   */
  if( gplen == olen )
    penalty = 0;

  dist = 1;
  if( nlen == olen ) {
    char *o = PREFIX_VARDATA(orig);
    char *n = PREFIX_VARDATA(new);
    dist    = abs((int)o[olen-1] - (int)n[nlen-1]);
  }
  penalty = (((float)dist) / powf(256, gplen));

#ifdef DEBUG_PENALTY
  elog(NOTICE, "gprefix_penalty_internal(%s, %s) == %d/(256^%d) == %g", 
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(orig))),
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(new))),
       dist, gplen, penalty);
#endif

  return penalty;
}

/**
 * For testing purposes we export our penalty function to SQL
 */
PG_FUNCTION_INFO_V1(prefix_penalty);
Datum
prefix_penalty(PG_FUNCTION_ARGS)
{
  float penalty = prefix_penalty_internal(PREFIX_PG_GETARG_TEXT(0),
					  PREFIX_PG_GETARG_TEXT(1));

  PG_RETURN_FLOAT4(penalty);
}

/**
 * GiST opclass methods
 */

PG_FUNCTION_INFO_V1(gprefix_consistent);
Datum
gprefix_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    text *query = (text *) PG_GETARG_POINTER(1);
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    text *key = (text *) DatumGetPointer(entry->key);
    bool retval;

    /**
     * We only have 1 Strategy (operator @>)
     * and we want to avoid compiler complaints that we do not use it.
     */ 
    Assert(strategy == 1);
    (void) strategy;
    retval = prefix_contains_internal(key, query, true);

    PG_RETURN_BOOL(retval);
}

/**
 * Prefix penalty: we want the penalty to be lower for closer
 * prefixes, taking into account length difference and content
 * distance.
 *
 * For examples we want new prefix 125 to be inserted by preference in
 * the 124 branch, not in a 128 or a 256 branch.
 *
 */
PG_FUNCTION_INFO_V1(gprefix_penalty);
Datum
gprefix_penalty(PG_FUNCTION_ARGS)
{
  GISTENTRY *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
  GISTENTRY *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
  float *penalty = (float *) PG_GETARG_POINTER(2);
  
  text *orig = (text *) DatumGetPointer(origentry->key);
  text *new  = (text *) DatumGetPointer(newentry->key);

  *penalty = prefix_penalty_internal(orig, new);
  PG_RETURN_POINTER(penalty);
}

/**
 * prefix picksplit first pass step: presort the SPLITVEC vector by
 * positionning the elements sharing the non-empty prefix which is the
 * more frequent in the distribution at the beginning of the vector.
 *
 * This will have the effect that the picksplit() implementation will
 * do a better job, per preliminary tests on not-so random data.
 */
struct gprefix_unions
{
  text *prefix;     /* a shared prefix */
  int   n;          /* how many entries begins with this prefix */
};


static inline
text **prefix_presort(GistEntryVector *list)
{
  GISTENTRY *ent      = list->vector;
  OffsetNumber maxoff = list->n - 1;
  text *init = (text *) DatumGetPointer(ent[FirstOffsetNumber].key);
  text *cur, *gp;
  int  gplen;
  bool found;

  struct gprefix_unions max;
  struct gprefix_unions *unions = (struct gprefix_unions *)
    palloc((maxoff+1) * sizeof(struct gprefix_unions));

  OffsetNumber unions_it = FirstOffsetNumber; /* unions iterator */
  OffsetNumber i, u;

  int result_it, result_it_maxes = FirstOffsetNumber;
  text **result = (text **)palloc((maxoff+1) * sizeof(text *));

#ifdef DEBUG_PRESORT_MAX
  int debug_count;
#endif
#ifdef DEBUG_PRESORT_UNIONS
  int debug_count;
#endif

  unions[unions_it].prefix = init;
  unions[unions_it].n      = 1;
  unions_it = OffsetNumberNext(unions_it);

  max.prefix = init;
  max.n      = 1;

#ifdef DEBUG_PRESORT_MAX
  elog(NOTICE, " prefix_presort():   init=%s max.prefix=%s max.n=%d",
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(init))),
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(max.prefix))),
       max.n);
#endif

  /**
   * Prepare a list of prefixes and how many time they are found.
   */
  for(i = OffsetNumberNext(FirstOffsetNumber); i <= maxoff; i = OffsetNumberNext(i)) {
    found = false;
    cur   = (text *) DatumGetPointer(ent[i].key);

    for(u = FirstOffsetNumber; u < unions_it; u = OffsetNumberNext(u)) {
      if( unions[u].n < 1 )
	continue;

      /**
       * We'll need the prefix itself, so it's better to call
       * greater_prefix_internal each time rather than
       * prefix_contains_internal then when true
       * greater_prefix_internal.
       */
      gp    = greater_prefix_internal(cur, unions[u].prefix);
      gplen = PREFIX_VARSIZE(gp);

#ifdef DEBUG_PRESORT_GP
      if( gplen > 0 ) {
	elog(NOTICE, " prefix_presort():   gplen=%2d, %s @> %s = %s",
	     gplen,
	     DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(gp))),
	     DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(cur))),
	     (prefix_contains_internal(gp, cur, true) ? "t" : "f"));
      }
#endif

      if( gplen > 0 ) {
	Assert(prefix_contains_internal(gp, cur, true));
      }

      if( gplen > 0 ) {
	/**
	 * Current list entry share a common prefix with some previous
	 * analyzed list entry, update the prefix and number.
	 */
	found = true;
	unions[u].n     += 1;
	unions[u].prefix = gp;

	/**
	 * We just updated unions, we may have to update max too.
	 */
	if( unions[u].n > max.n ) {
	  max.prefix = unions[u].prefix;
	  max.n      = unions[u].n;
#ifdef DEBUG_PRESORT_MAX
  elog(NOTICE, " prefix_presort():   max.prefix=%s max.n=%d",
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(max.prefix))),
       max.n);	  
#endif
	}
	
	/**
	 * break from the unions loop, we're done with it for this
	 * element.
	 */
	break;
      }
    }
    /**
     * We're done with the unions loop, if we didn't find a common
     * prefix we have to add the current list element to unions
     */
    if( !found ) {
      unions[unions_it].prefix = cur;
      unions[unions_it].n      = 1;
      unions_it = OffsetNumberNext(unions_it);
    }
  }
#ifdef DEBUG_PRESORT_UNIONS
  debug_count = 0;
  for(u = FirstOffsetNumber; u < unions_it; u = OffsetNumberNext(u)) {
    debug_count += unions[u].n;
    elog(NOTICE, " prefix_presort():   unions[%s] = %d", 
	 DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(unions[u].prefix))),
	 unions[u].n);
  }
  elog(NOTICE, " prefix_presort():   total: %d", debug_count);
#endif

#ifdef DEBUG_PRESORT_MAX
  debug_count = 0;
  for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
    cur   = (text *) DatumGetPointer(ent[i].key);

    if( prefix_contains_internal(max.prefix, cur, true) )
      debug_count++;
  }
  elog(NOTICE, " prefix_presort():   max.prefix %s @> %d entries",
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(max.prefix))),
       debug_count);
#endif

  /**
   * We now have a list of common non-empty prefixes found on the list
   * (unions) and kept the max entry while computing this weighted
   * unions list.
   *
   * Simple case : a common non-empty prefix is shared by all list
   * entries.
   */
  if( max.n == list->n ) {
    /**
     * A common non-empty prefix is shared by all list entries.
     */
    for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
      cur = (text *) DatumGetPointer(ent[i].key);
      result[i] = cur;
    }
    return result;
  }

  /**
   * If we arrive here, we now have to make up the result by copying
   * max matching elements first, then the others list entries in
   * their original order. To do this, we reserve the first result
   * max.n places to the max.prefix matching elements (see result_it
   * and result_it_maxes).
   *
   * result_it_maxes will go from FirstOffsetNumber to max.n included,
   * and result_it will iterate through the end of the list, that is
   * from max.n - FirstOffsetNumber + 1 to maxoff.
   *
   * [a, b] contains b - a + 1 elements, hence
   * [FirstOffsetNumber, max.n] contains max.n - FirstOffsetNumber + 1
   * elements, whatever FirstOffsetNumber value.
   */
  result_it_maxes = FirstOffsetNumber;
  result_it       = OffsetNumberNext(max.n - FirstOffsetNumber + 1);

#ifdef DEBUG_PRESORT_MAX
  elog(NOTICE, " prefix_presort():   max.prefix=%s max.n=%d result_it=%d",
       DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(max.prefix))),
       max.n, result_it);
#endif

  for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
    cur = (text *) DatumGetPointer(ent[i].key);

#ifdef DEBUG_PRESORT_RESULT
    elog(NOTICE, " prefix_presort():   ent[%4d] = %s <@ %s = %s => result[%4d]", 
	 i,
	 DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(cur))),
	 DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(max.prefix))),
	 (prefix_contains_internal(max.prefix, cur, true) ? "t" : "f"),
	 (prefix_contains_internal(max.prefix, cur, true) ? result_it_maxes : result_it));
#endif

    if( prefix_contains_internal(max.prefix, cur, true) ) {
      /**
       * cur has to go in first part of the list, as max.prefix is a
       * prefix of it.
       */
      Assert(result_it_maxes <= max.n);
      result[result_it_maxes] = cur;
      result_it_maxes = OffsetNumberNext(result_it_maxes);
    }
    else {
      /**
       * cur has to go at next second part position.
       */
      Assert(result_it <= maxoff);
      result[result_it] = cur;
      result_it = OffsetNumberNext(result_it);
    }
  }
#ifdef DEBUG_PRESORT_RESULT
  elog(NOTICE, " prefix_presort():   result_it_maxes=%4d result_it=%4d list->n=%d maxoff=%d",
       result_it_maxes, result_it, list->n, maxoff);
#endif
  return result;
}



/**
 * prefix picksplit implementation
 *
 * The idea is to consume the SPLITVEC vector by both its start and
 * end, inserting one or two items at a time depending on relative
 * penalty() with current ends of new vectors, or even all remaining
 * items at once.
 *
 * Idea and perl test script per AndreSN with some modifications by me
 * (Dimitri Fontaine).
 *
 * TODO: check whether qsort() is the right first pass. Another idea
 * (by dim, this time) being to care first about items which non-empty
 * union appears the most in the SPLITVEC vector. Perl test
 * implementation show good results on random test data.
 */

PG_FUNCTION_INFO_V1(gprefix_picksplit);
Datum
gprefix_picksplit(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    OffsetNumber maxoff = entryvec->n - 1;
    GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

    int	nbytes;
    OffsetNumber offl, offr;
    OffsetNumber *listL;
    OffsetNumber *listR;
    text *curl, *curr, *gp;
    text *unionL;
    text *unionR;
    
    /**
     * Keeping track of penalties to insert into ListL or ListR, for
     * both the leftmost and the rightmost element of the remaining
     * list.
     */
    float pll, plr, prl, prr;

    /**
     * First pass: sort out the entryvec.
     */
    text **sorted = prefix_presort(entryvec);

    nbytes = (maxoff + 1) * sizeof(OffsetNumber);
    listL = (OffsetNumber *) palloc(nbytes);
    listR = (OffsetNumber *) palloc(nbytes);
    v->spl_left  = listL;
    v->spl_right = listR;
    v->spl_nleft = v->spl_nright = 0;

    offl = FirstOffsetNumber;
    offr = maxoff;

    unionL = sorted[offl];
    unionR = sorted[offr];

    v->spl_left[v->spl_nleft++]   = offl;
    v->spl_right[v->spl_nright++] = offr;
    v->spl_left  = listL;
    v->spl_right = listR;

    offl = OffsetNumberNext(offl);
    offr = OffsetNumberPrev(offr);

    for(; offl < offr; offl = OffsetNumberNext(offl), offr = OffsetNumberPrev(offr)) {
      curl = sorted[offl];
      curr = sorted[offr];

      Assert(curl != NULL && curr != NULL);

      pll = prefix_penalty_internal(unionL, curl);
      plr = prefix_penalty_internal(unionR, curl);
      prl = prefix_penalty_internal(unionL, curr);
      prr = prefix_penalty_internal(unionR, curr);

      if( pll <= plr && prl >= prr ) {
	/**
	 * curl should go to left and curr to right, unless they share
	 * a non-empty common prefix, in which case we place both curr
	 * and curl on the same side. Arbitrarily the left one.
	 */
	if( pll == plr && prl == prr ) {
	  gp = greater_prefix_internal(curl, curr);
	  if( PREFIX_VARSIZE(gp) > 0 ) {
	    unionL = greater_prefix_internal(unionL, gp);
	    v->spl_left[v->spl_nleft++] = offl;
	    v->spl_left[v->spl_nleft++] = offr;
	    continue;
	  }
	}
	/**
	 * here pll <= plr and prl >= prr and (pll != plr || prl != prr)
	 */
	unionL = greater_prefix_internal(unionL, curl);
	unionR = greater_prefix_internal(unionR, curr);
	v->spl_left[v->spl_nleft++]   = offl;
	v->spl_right[v->spl_nright++] = offr;
      }
      else if( pll > plr && prl >= prr ) {
	unionR = greater_prefix_internal(unionR, curr);
	v->spl_right[v->spl_nright++] = offr;
      }
      else if( pll <= plr && prl < prr ) {
	/**
	 * Current leftmost entry is added to listL
	 */
	unionL = greater_prefix_internal(unionL, curl);
	v->spl_left[v->spl_nleft++] = offl;
      }
      else if( (pll - plr) < (prr - prl) ) {
	/**
	 * All entries still in the list go into listL
	 */
	for(; offl <= maxoff; offl = OffsetNumberNext(offl)) {
	  curl   = sorted[offl];
	  unionL = greater_prefix_internal(unionL, curl);
	  v->spl_left[v->spl_nleft++] = offl;
	}
      }
      else {
	/**
	 * All entries still in the list go into listR
	 */
	for(; offl <= maxoff; offl = OffsetNumberNext(offl)) {
	  curl   = sorted[offl];
	  unionR = greater_prefix_internal(unionR, curl);
	  v->spl_right[v->spl_nright++] = offl;
	}
      }
    }

    /**
     * The for loop continues while offl < offr. If maxoff is odd, it
     * could be that there's a last value to process. Here we choose
     * where to add it.
     */
    if( offl == offr ) {
      curl = sorted[offl];
      pll  = prefix_penalty_internal(unionL, curl);
      plr  = prefix_penalty_internal(unionR, curl);

      if( pll < plr || (pll == plr && v->spl_nleft < v->spl_nright) ) {
	curl   = sorted[offl];
	unionL = greater_prefix_internal(unionL, curl);
	v->spl_left[v->spl_nleft++] = offl;
      }
      else {
	curl   = sorted[offl];
	unionR = greater_prefix_internal(unionR, curl);
	v->spl_right[v->spl_nright++] = offl;
      }
    }

    v->spl_ldatum = PointerGetDatum(unionL);
    v->spl_rdatum = PointerGetDatum(unionR);

    /**
     * All read entries (maxoff) should have make it to the
     * GIST_SPLITVEC return value.
     */
    Assert(maxoff = v->spl_nleft+v->spl_nright);

#ifdef DEBUG
    elog(NOTICE, "gprefix_picksplit(): entryvec->n=%4d maxoff=%4d l=%4d r=%4d l+r=%4d unionL=%s unionR=%s",
	 entryvec->n, maxoff, v->spl_nleft, v->spl_nright, v->spl_nleft+v->spl_nright,
	 DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(unionL))),
	 DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(unionR))));
#endif
	
    PG_RETURN_POINTER(v);
}

/**
 * Prefix union should return the greatest common prefix.
 */
PG_FUNCTION_INFO_V1(gprefix_union);
Datum
gprefix_union(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    GISTENTRY *ent = entryvec->vector;

    text *out, *tmp, *gp;
    int	numranges, i = 0;

    numranges = entryvec->n;
    tmp = (text *) DatumGetPointer(ent[0].key);
    out = tmp;

    if( numranges == 1 ) {
      /**
       * We need to return a palloc()ed copy of ent[0].key (==tmp)
       */
      out = DatumGetTextPCopy(PointerGetDatum(tmp));
#ifdef DEBUG_UNION
      elog(NOTICE, "gprefix_union(%s) == %s",
	   DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(tmp))),
	   DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(out))));
#endif
      PG_RETURN_POINTER(out);
    }
  
    for (i = 1; i < numranges; i++) {
      tmp = (text *) DatumGetPointer(ent[i].key);
      gp = greater_prefix_internal(out, tmp);
#ifdef DEBUG_UNION
      elog(NOTICE, "gprefix_union: gp(%s, %s) == %s", 
	   DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(out))),
	   DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(tmp))),
	   DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(gp))));
#endif
      out = gp;
    }

#ifdef DEBUG_UNION
    elog(NOTICE, "gprefix_union: %s", 
	 DatumGetCString(DirectFunctionCall1(textout,PointerGetDatum(out))));
#endif

    PG_RETURN_POINTER(out);
}

/**
 * GiST Compress and Decompress methods for prefix
 * do not do anything.
 */
PG_FUNCTION_INFO_V1(gprefix_compress);
Datum
gprefix_compress(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(gprefix_decompress);
Datum
gprefix_decompress(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/**
 * Equality methods
 */
PG_FUNCTION_INFO_V1(gprefix_same);
Datum
gprefix_same(PG_FUNCTION_ARGS)
{
    text *v1 = (text *) PG_GETARG_POINTER(0);
    text *v2 = (text *) PG_GETARG_POINTER(1);
    bool *result = (bool *) PG_GETARG_POINTER(2);

    *result = DirectFunctionCall2(texteq,
				  PointerGetDatum(v1), 
				  PointerGetDatum(v2));

    PG_RETURN_POINTER(result);    
}

