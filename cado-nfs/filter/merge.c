/* merge --- new merge program

Copyright 2019-2020 Charles Bouillaguet and Paul Zimmermann.

This file is part of CADO-NFS.

CADO-NFS is free software; you can redistribute it and/or modify it under the
terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option)
any later version.

CADO-NFS is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
details.

You should have received a copy of the GNU Lesser General Public License
along with CADO-NFS; see the file COPYING.  If not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include "cado.h" // IWYU pragma: keep
/* the following should come after cado.h, which sets -Werror=all */
#ifdef  __GNUC__
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>       // for PRIu64
#include <stdint.h>         // for uint64_t, int32_t, int64_t, uint32_t
#include <string.h>         // for memset, memcpy, strlen
#ifdef HAVE_MINGW
#include <fcntl.h>         /* for _O_BINARY */
#endif
#include "filter_io.h"  // earlyparsed_relation_ptr
#ifdef FOR_DL
#include "gcd.h"
#endif
#include "gzip.h"       // fopen_maybe_compressed
#include "macros.h"
#include "memory.h"             // malloc_aligned
#include "memusage.h"   // PeakMemusage
#include "misc.h"       // UMAX
#include "mst.h"
#include "omp_proxy.h"    // verbose_interpret_parameters
#include "params.h"     // param_list_parse_*
#include "purgedfile.h"     // for purgedfile_read_firstline
#include "report.h"     /* for report_t */
#include "sparse.h"
#include "timing.h"  // seconds
#include "typedefs.h"  // weight_t
#include "verbose.h"    // verbose_interpret_parameters


int pass = 0;

/* a lot of verbosity */
// #define BIG_BROTHER

/* some more verbosity which requires additional operations */
// #define BIG_BROTHER_EXPENSIVE

#ifdef BIG_BROTHER
    unsigned char *touched_columns = NULL;
#endif

/* define CANCEL to count column cancellations */
// #define CANCEL
#ifdef CANCEL
#define CANCEL_MAX 2048
unsigned long cancel_rows = 0;
unsigned long cancel_cols[CANCEL_MAX] = {0,};
#endif

/* define DEBUG if printRow or copy_matrix is needed */
// #define DEBUG

/* CBOUND_INCR is the increment on the maximal cost of merges at each step.
   Setting it to 1 is optimal in terms of matrix size, but will take a very
   long time (typically 10 times more than with CBOUND_INCR=10).
   The following values were determined experimentally. */
#ifndef FOR_DL
#define CBOUND_INCR 13
#else
#define CBOUND_INCR 31
#endif


/* Note about variables used in the code:
 * cwmax is the (current) maximal weight of columns that will be considered
   for a merge. It starts at cwmax=2. Once we have performed *all* 2-merges,
   we increase cwmax to 3, and at each step of the algorithm, we increase it
   by 1 (not waiting for all 3-merges to be completed).
 * cbound is the maximum (current) fill-in that is allowed for a merge
   (in fact, it is a biased value to avoid negative values, one should subtract
    BIAS from cbound to get the actual value). It starts at 0, and once all
    the 2-merges have been performed (which all give a negative fill-in, thus
    they will all be allowed), we increase cbound by CBOUND_INCR at each step
    of the algorithm (where CBOUND_INCR differs for integer factorization and
    discrete logarithm).
 * j0 means that we assume that columns of index < j0 cannot have
   weight <= cwmax. It depends on cwmax (decreases when cwmax increases).
   At the first call to compute_weights(), the values j0(cwmax=2) up to
   j0(MERGE_LEVEL_MAX) are computed once for all (since the weight of a
   column usually does not decrease, the values of j0 should remain correct
   during the algorithm, but not optimal).
   In several places we use the fact that the rows are sorted by increasing
   columns: if we start from the end, we can stop at soon as j < j0.
*/

#define COMPUTE_W  0 /* compute_weights */
#define COMPUTE_R  1 /* compute_R */
#define COMPUTE_M  2 /* compute_merges */
#define APPLY_M    3 /* apply_merges */
#define PASS       4 /* pass */
#define RECOMPRESS 5 /* recompress */
#define FLUSH      6 /* buffer_flush */
#define GC         7 /* garbage collection */
double cpu_t[8] = {0};
double wct_t[8] = {0};

static int verbose = 0; /* verbosity level */

#define MARGIN 5 /* reallocate dynamic lists with increment 1/MARGIN */

// #define TRACE_J 1438672   for debuging purposes


static void
print_timings (char *s, double cpu, double wct)
{
  printf ("%s %.1fs (cpu), %.1fs (wct) [cpu/wct=%.1f]\n",
	  s, cpu, wct, cpu / wct);
  fflush (stdout);
}

/*************************** output buffer ***********************************/

typedef struct {
  char* buf;
  size_t size;  /* used size */
  size_t alloc; /* allocated size */
} buffer_struct_t;

static buffer_struct_t*
buffer_init (int nthreads)
{
  buffer_struct_t *Buf;
  Buf = malloc (nthreads * sizeof (buffer_struct_t));
  for (int i = 0; i < nthreads; i++)
    {
      Buf[i].buf = NULL;
      Buf[i].size = 0;
      Buf[i].alloc = 0;
    }
  return Buf;
}

static void
buffer_add (buffer_struct_t *buf, char *s)
{
  size_t n = strlen (s) + 1; /* count final '\0' */
  if (buf->size + n > buf->alloc)
    {
      buf->alloc = buf->size + n;
      buf->alloc += buf->alloc / MARGIN;
      buf->buf = realloc (buf->buf, buf->alloc * sizeof (char));
    }
  memcpy (buf->buf + buf->size, s, n * sizeof (char));
  buf->size += n - 1; /* don't count final '\0' */
}

static void
buffer_flush (buffer_struct_t *Buf, int nthreads, FILE *out)
{
  double cpu = seconds (), wct = wct_seconds ();
  for (int i = 0; i < nthreads; i++)
    {
      /* it is important to check whether size=0, otherwise the previous
         buffer will be printed twice */
      if (Buf[i].size != 0)
        fprintf (out, "%s", Buf[i].buf);
      Buf[i].size = 0;
    }
  cpu = seconds () - cpu;
  wct = wct_seconds () - wct;
  print_timings ("   buffer_flush took", cpu, wct);
  cpu_t[FLUSH] += cpu;
  wct_t[FLUSH] += wct;
}

static void
buffer_clear (buffer_struct_t *Buf, int nthreads)
{
  for (int i = 0; i < nthreads; i++)
    free (Buf[i].buf);
  free (Buf);
}

/*************************** heap structures *********************************/

/* Allocates and garbage-collects relations (i.e. arrays of typerow_t).

  Threads allocate PAGES of memory of a fixed size to store rows, and each
  thread has a single ACTIVE page in which it writes new rows. When the active
  page is FULL, the thread grabs an EMPTY page that becomes its new active page.
  In each page, there is a pointer [[ptr]] to the begining of the free space. To
  allocate [[b]] bytes for a new row, it suffices to note the current value of
  [[ptr]] and then to increase it by [[b]] --- if this would overflow the current
  page, then it is marked as "full" and a new active page is obtained. Rows are
  stored along with their number, their size and the list of their coefficients.
  To delete a row, we just mark it as deleted by setting its number to -1. Thus,
  row allocation and deallocation are thread-local operations that are very
  fast. The last allocated row can easily be shrunk (by diminishing [[ptr]]).

  After each pass, memory is garbage-collected. All threads do the following
  procedure in parallel, while possible: grab a full page that was not created
  during this pass; copy all non-deleted rows to the current active page; mark the
  old full page as "empty". Note that moving row $i$ to a different address
  in memory requires an update to the ``row pointer'' in [[mat]]; this is
  why rows are stored along with their number. When they need a new page, threads
  first try to grab an existing empty page. If there is none, a new page is
  allocated from the OS. There are global doubly-linked lists of full and empty
  pages, protected by a lock, but these are infrequently accessed.
*/

#define PAGE_SIZE ((1<<18) - 4) /* seems to be optimal for RSA-512 */

struct page_t {
        struct pagelist_t *list;     /* the pagelist_t structure associated with this page */
        int i;                       /* page number, for debugging purposes */
        int generation;              /* pass in which this page was filled. */
        int ptr;                     /* data[ptr:PAGE_SIZE] is available*/
        typerow_t data[PAGE_SIZE];
};

// linked list of pages (doubly-linked for the full pages, simply-linked for the empty pages)
struct pagelist_t {
        struct pagelist_t *next;
        struct pagelist_t *prev;
        struct page_t *page;
};

int n_pages, n_full_pages, n_empty_pages;
struct pagelist_t headnode;                  // dummy node for the list of full pages
struct pagelist_t *full_pages, *empty_pages; // head of the page linked lists

struct page_t **active_page; /* active page of each thread */
long long *heap_waste;       /* space wasted, per-thread. Can be negative! The sum over all threads is correct. */


/* provide an empty page */
static struct page_t *
heap_get_free_page()
{
        struct page_t *page = NULL;
        #pragma omp critical(pagelist)
        {
                // try to grab it from the simply-linked list of empty pages.
                if (empty_pages != NULL) {
                        page = empty_pages->page;
                        empty_pages = empty_pages->next;
                        n_empty_pages--;
                } else {
                        n_pages++;   /* we will malloc() it, update count while still in critical section */
                }
        }
        if (page == NULL) {
                // we must allocate a new page from the OS.
                page = malloc(sizeof(struct page_t));
                struct pagelist_t *item = malloc(sizeof(struct pagelist_t));
                page->list = item;
                item->page = page;
                page->i = n_pages;
        }
        page->ptr = 0;
        page->generation = pass;
        return page;
}

/* provide the oldest full page with generation < max_generation, or NULL if none is available,
   and remove it from the doubly-linked list of full pages */
static struct page_t *
heap_get_full_page(int max_generation)
{
        struct pagelist_t *item = NULL;
        struct page_t *page = NULL;
        #pragma omp critical(pagelist)
        {
                item = full_pages->next;
                if (item->page != NULL && item->page->generation < max_generation) {
                        page = item->page;
                        item->next->prev = item->prev;
                        item->prev->next = item->next;
                        n_full_pages--;
                }
        }
        return page;
}


/* declare that the given page is empty */
static  void
heap_clear_page(struct page_t *page)
{
        struct pagelist_t *item = page->list;
        #pragma omp critical(pagelist)
        {
                item->next = empty_pages;
                empty_pages = item;
                n_empty_pages++;
        }
}

/* declare that the given page is full. Insert to the left of the list of full pages.
   The list is sorted (following next) by increasing generation. */
static void
heap_release_page(struct page_t *page)
{
        struct pagelist_t *list = page->list;
        struct pagelist_t *target;
        #pragma omp critical(pagelist)
        {
                target = full_pages->prev;
                while (target->page != NULL && page->generation < target->page->generation)
                        target = target->prev;
                list->next = target;
                list->prev = target->prev;
                list->next->prev = list;
                list->prev->next = list;
                n_full_pages++;
        }
}

// set up the page linked lists
static void
heap_setup()
{
        // setup the doubly-linked list of full pages.
        full_pages = &headnode;
        full_pages->page = NULL;
        full_pages->next = full_pages;
        full_pages->prev = full_pages;

        empty_pages = NULL;
        int T = omp_get_max_threads();
        active_page = malloc(T * sizeof(*active_page));
        heap_waste = malloc(T * sizeof(*heap_waste));

        #pragma omp parallel for
        for(int t = 0 ; t < T ; t++) {
            active_page[t] = heap_get_free_page();
            heap_waste[t] = 0;
        }
}

/* release all memory. This is technically not necessary, because the "malloc"
   allocations are internal to the process, and all space allocated to the
   process is reclaimed by the OS on termination. However, doing this enables
   valgrind to check the absence of leaks.
*/
static void
heap_clear ()
{
  /* clear active pages */
  int T = omp_get_max_threads ();
  for (int t = 0 ; t < T ; t++) {
    free(active_page[t]->list);
    free(active_page[t]);
  }

  /* clear empty pages */
  while (empty_pages != NULL) {
    struct pagelist_t *item = empty_pages;
    empty_pages = item->next;
    free(item->page);
    free(item);
  }

  /* clear full pages. 1. Locate dummy node */
  while (full_pages->page != NULL)
    full_pages = full_pages->next;

  // 2. Skip dummy node
  full_pages = full_pages->next;

  // 3. Walk list until dummy node is met again, free everything.
  while (full_pages->page != NULL) {
    struct pagelist_t *item = full_pages;
    full_pages = full_pages->next;
    free(item->page);
    free(item);
  }
  free (active_page);
  free (heap_waste);
}


/* Returns a pointer to allocated space holding a size-s array of typerow_t.
   This function is thread-safe thanks to the threadprivate directive above. */
static inline typerow_t *
heap_malloc (size_t s)
{
  ASSERT(s <= PAGE_SIZE);
  int t = omp_get_thread_num();
  struct page_t *page = active_page[t];
  // ASSERT(page != NULL);
  /* enough room in active page ?*/
  if (page->ptr + s >= PAGE_SIZE) {
        heap_release_page(page);
        page = heap_get_free_page();
        active_page[t] = page;
  }
  typerow_t *alloc = page->data + page->ptr;
  page->ptr += s;
  return alloc;
}

/* Allocate space for row i, holding s coefficients in row[1:s+1] (row[0] == s).
   This writes s in row[0]. The size of the row must not change afterwards. Thread-safe.
*/
static inline typerow_t *
heap_alloc_row (index_t i, size_t s)
{
  typerow_t *alloc = heap_malloc(s + 2);
  rowCell(alloc, 0) = i;
  rowCell(alloc, 1) = s;
  return alloc + 1;
}

/* Shrinks the row. It must be the last one allocated by this thread. Thread-safe. */
static void
heap_resize_last_row (typerow_t *row, index_t new_size)
{
  int t = omp_get_thread_num();
  struct page_t *page = active_page[t];
  index_t old_size = rowCell(row, 0);
  ASSERT(row + old_size + 1 == page->data + page->ptr);
  int delta = old_size - new_size;
  rowCell(row, 0) = new_size;
  page->ptr -= delta;
}


/* given the pointer provided by heap_alloc_row, mark the row as deleted.
   Thread-safe */
static inline void
heap_destroy_row (typerow_t *row)
{
  int t = omp_get_thread_num();
  rowCell(row, -1) = (index_signed_t) -1;
  heap_waste[t] += rowCell(row, 0) + 2;
}

/* Copy non-garbage data to the active page of the current thread, then
   return the page to the freelist. Thread-safe.
   Warning: this may fill the current active page and release it. */
static int
collect_page(filter_matrix_t *mat, struct page_t *page)
{
        int garbage = 0;
        int bot = 0;
        int top = page->ptr;
        typerow_t *data = page->data;
        while (bot < top) {
                index_signed_t i = rowCell(data, bot);
                typerow_t *old = data + bot + 1;
                index_t size = rowCell(old, 0);
                if (i == (index_signed_t) -1) {
                        garbage += size + 2;
                } else {
                        ASSERT(mat->rows[i] == old);
                        typerow_t * new = heap_alloc_row(i, size);
                        memcpy(new, old, (size + 1) * sizeof(typerow_t));
                        setCell(new, -1, rowCell(old, -1), 0);
                        mat->rows[i] = new;
                }
                bot += size + 2;
        }
        int t = omp_get_thread_num();
        heap_waste[t] -= garbage;
        heap_clear_page(page);
        return garbage;
}

static double
heap_waste_ratio()
{
        int T = omp_get_max_threads();
        long long total_waste = 0;
        for (int t = 0; t < T; t++)
                total_waste += heap_waste[t];
        double waste = ((double) total_waste) / (n_pages - n_empty_pages) / PAGE_SIZE;
        return waste;
}

/* examine every full pages not created during the current pass and reclaim all lost space */
static void
full_garbage_collection(filter_matrix_t *mat)
{
        double cpu8 = seconds (), wct8 = wct_seconds ();
        double waste = heap_waste_ratio();
        printf("Starting collection with %.0f%% of waste...", 100 * waste);
        fflush(stdout);

        // I don't want to collect pages just filled during the collection
        int max_generation = pass;

        int i = 0;
        int initial_full_pages = n_full_pages;
        struct page_t *page;
        long long collected_garbage = 0;
        #pragma omp parallel reduction(+:i, collected_garbage) private(page)
        while ((page = heap_get_full_page(max_generation)) != NULL) {
                collected_garbage += collect_page(mat, page);
                i++;
        }

        double page_ratio = (double) i / initial_full_pages;
        double recycling = 1 - heap_waste_ratio() / waste;
        printf("Examined %.0f%% of full pages, recycled %.0f%% of waste. %.0f%% of examined data was garbage\n",
        	100 * page_ratio, 100 * recycling, 100.0 * collected_garbage / i / PAGE_SIZE);

        cpu8 = seconds () - cpu8;
        wct8 = wct_seconds () - wct8;
        print_timings ("   GC took", cpu8, wct8);
        cpu_t[GC] += cpu8;
        wct_t[GC] += wct8;
}

/*****************************************************************************/

static void
declare_usage(param_list pl)
{
  param_list_decl_usage(pl, "mat", "input purged file");
  param_list_decl_usage(pl, "out", "output history file");
  param_list_decl_usage(pl, "skip", "number of heavy columns to bury (default "
				    CADO_STRINGIZE(DEFAULT_MERGE_SKIP) ")");
  param_list_decl_usage(pl, "target_density", "stop when the average row density exceeds this value"
			    " (default " CADO_STRINGIZE(DEFAULT_MERGE_TARGET_DENSITY) ")");
  param_list_decl_usage(pl, "force-posix-threads", "force the use of posix threads, do not rely on platform memory semantics");
  param_list_decl_usage(pl, "path_antebuffer", "path to antebuffer program");
  param_list_decl_usage(pl, "t", "number of threads");
  param_list_decl_usage(pl, "v", "verbose mode");
}

static void
usage (param_list pl, char *argv0)
{
    param_list_print_usage(pl, argv0, stderr);
    exit(EXIT_FAILURE);
}



#ifndef FOR_DL
/* sort row[0], row[1], ..., row[n-1] in non-decreasing order */
static void
sort_relation (index_t *row, unsigned int n)
{
  unsigned int i, j;

  for (i = 1; i < n; i++)
    {
      index_t t = row[i];
      if (t < row[i-1])
	{
	  row[i] = row[i-1];
	  for (j = i - 1; j > 0 && t < row[j-1]; j--)
	    row[j] = row[j-1];
	  row[j] = t;
	}
    }
}
#endif

/* callback function called by filter_rels */
static void *
insert_rel_into_table (void *context_data, earlyparsed_relation_ptr rel)
{
  filter_matrix_t *mat = (filter_matrix_t *) context_data;
  unsigned int j = 0;
  typerow_t buf[UMAX(weight_t)]; /* rel->nb is of type weight_t */

  for (unsigned int i = 0; i < rel->nb; i++)
  {
    index_t h = rel->primes[i].h;
    mat->rem_ncols += (mat->wt[h] == 0);
    mat->wt[h] += (mat->wt[h] != UMAX(col_weight_t));
    if (h < mat->skip)
	continue; /* we skip (bury) the first 'skip' indices */
#ifdef FOR_DL
    exponent_t e = rel->primes[i].e;
    /* For factorization, they should not be any multiplicity here.
       For DL we do not want to count multiplicity in mat->wt */
    buf[++j] = (ideal_merge_t) {.id = h, .e = e};
#else
    ASSERT(rel->primes[i].e == 1);
    buf[++j] = h;
#endif
  }

#ifdef FOR_DL
  buf[0].id = j;
#else
  buf[0] = j;
#endif

  /* only count the non-skipped coefficients */
  mat->tot_weight += j;

  /* sort indices to ease row merges */
#ifndef FOR_DL
  sort_relation (&(buf[1]), j);
#else
  qsort (&(buf[1]), j, sizeof(typerow_t), cmp_typerow_t);
#endif

  mat->rows[rel->num] = heap_alloc_row(rel->num, j);
  compressRow (mat->rows[rel->num], buf, j);  /* sparse.c, simple copy loop... */

  return NULL;
}

static void
filter_matrix_read (filter_matrix_t *mat, const char *purgedname)
{
  uint64_t nread;
  char *fic[2] = {(char *) purgedname, NULL};

  /* read all rels */
  nread = filter_rels (fic, (filter_rels_callback_t) &insert_rel_into_table,
		       mat, EARLYPARSE_NEED_INDEX, NULL, NULL);
  ASSERT_ALWAYS(nread == mat->nrows);
  mat->rem_nrows = nread;
}



/* stack non-empty columns at the begining. Update mat->p (for DL) and jmin */
static void recompress(filter_matrix_t *mat, index_t *jmin)
{
	double cpu = seconds (), wct = wct_seconds ();
	uint64_t nrows = mat->nrows;
	uint64_t ncols = mat->ncols;

	/* sends the old column number to the new one */
	index_t *p = malloc(ncols * sizeof(*p));

        /* new column weights */
        col_weight_t *nwt = malloc(mat->rem_ncols * sizeof(*nwt));

        /* compute the number of non-empty columns */
        {
            /* need an array that is visible to all threads in order to do the
             * prefix sum. Wish I knew another way.
             */
            int T = omp_get_max_threads();
            index_t tm[T]; /* #non-empty columns seen by thread t */
#pragma omp parallel
            {
                int T = omp_get_num_threads();
                int t = omp_get_thread_num();
                index_t m = 0;
#pragma omp for schedule(static) nowait /* static is mandatory here */
                for (index_t j = 0; j < ncols; j++)
                    if (0 < mat->wt[j])
                        m++;
                tm[t] = m;

#pragma omp barrier

                /* prefix-sum over the T threads (sequentially) */
#pragma omp single
                {
                    index_t s = 0;
                    for (int t = 0; t < T; t++) {
                        index_t m = tm[t];
                        tm[t] = s;
                        s += m;
                    }
                    /* we should have s = mat->rem_ncols now, thus no need
                       to copy s into mat->rem_ncols, but it appears in
                       some cases it does not hold (cf
https://cado-nfs-ci.loria.fr/ci/job/future-parallel-merge/job/compile-debian-testing-amd64-large-pr/147/) */
                    mat->rem_ncols = s;
                }

                /* compute the new column indices */
                m = tm[t];
#pragma omp for schedule(static) /* static is mandatory here */
                for (index_t j = 0; j < ncols; j++) {
                    ASSERT(m <= j);
                    p[j] = m;
                    if (0 < mat->wt[j])
                        m++;
                }

                /* rewrite the row indices */
#pragma omp for schedule(guided) /* guided is slightly better than static */
                for (uint64_t i = 0; i < nrows; i++) {
                    if (mat->rows[i] == NULL) 	/* row was discarded */
                        continue;
                    for (index_t l = 1; l <= matLengthRow(mat, i); l++)
                        matCell(mat, i, l) = p[matCell(mat, i, l)];
                }

                /* update mat->wt */
#pragma omp for schedule(static) /* static is slightly better than guided */
                for (index_t j = 0; j < ncols; j++)
                    if (0 < mat->wt[j])
                        nwt[p[j]] = mat->wt[j];

            } /* end parallel section */
        }

        #ifdef FOR_DL
        /* For the discrete logarithm, we keep the inverse of p, to print the
	original columns in the history file.
	Warning: for a column j of weight 0, we have p[j] = p[j'] where
	j' is the smallest column > j of positive weight, thus we only consider
	j such that p[j] < p[j+1], or j = ncols-1. */
        if (mat->p == NULL) {
        	mat->p = malloc(mat->rem_ncols * sizeof (index_t));
                /* We must pay attention to the case of empty columns at the
                 * end */
		for (uint64_t i = 0, j = 0; j < mat->ncols && i < mat->rem_ncols; j++) {
                    if (p[j] == i && (j + 1 == mat->ncols || p[j] < p[j+1]))
                        mat->p[i++] = j; /* necessarily i <= j */
                }
        } else {
	/* update mat->p. It sends actual indices in mat to original indices in the purge file */
        // before : mat->p[i] == original
        //  after : mat->p[p[i]] == original
        /* Warning: in multi-thread mode, one should take care not to write
           some mat->p[j] before it is used by another thread.
           Consider for example ncols = 4 with 2 threads, and active
           columns 1 and 2. Then we have p[1] = 0 and p[2] = 1.
           Thus thread 0 executes mat->p[0] = mat->p[1], and thread 1 executes
           mat->p[1] = mat->p[2]. If thread 1 is ahead of thread 0, the final
           value of mat->p[0] will be wrong (it will be the initial value of
           mat->p[2], instead of the initial value of mat->p[1]).
           To solve that problem, we store the new values in another array. */
        	index_t *new_p = malloc (mat->rem_ncols * sizeof (index_t));
		/* static slightly better than guided for the following loop */
                #pragma omp for schedule(static)
		for (index_t j = 0; j < ncols; j++)
			if (0 < mat->wt[j])
				new_p[p[j]] = mat->p[j];
		free(mat->p);
		mat->p = new_p;
        }
        #endif

	free(mat->wt);
	mat->wt = nwt;

	/* update jmin */
	if (jmin[0] == 1)
                for (int w = 1; w <= MERGE_LEVEL_MAX; w++)
                /* Warning: we might have jmin[w] = ncols. */
                        jmin[w] = (jmin[w] < ncols) ? p[jmin[w]] : mat->rem_ncols;

	free(p);

	/* this was the goal all along! */
	mat->ncols = mat->rem_ncols;
	cpu = seconds () - cpu;
	wct = wct_seconds () - wct;
	print_timings ("   recompress took", cpu, wct);
	cpu_t[RECOMPRESS] += cpu;
	wct_t[RECOMPRESS] += wct;
}


/* For 1 <= w <= MERGE_LEVEL_MAX, put in jmin[w] the smallest index j such that
   mat->wt[j] = w. This routine is called only once, at the first call of
   compute_weights. */

static void
compute_jmin (filter_matrix_t *mat, index_t *jmin)
{
    {
        /* unfortunately, reduction on array sections requires OpenMP >= 4.5,
           which is not yet THAT widespread. We work around the problem */
        /* TODO: I wonder which openmp level we require anyway. Maybe
         * it's already 4.5+ */
        index_t tjmin[omp_get_max_threads()][MERGE_LEVEL_MAX + 1];

#pragma omp parallel /* reduction(min: jmin[1:MERGE_LEVEL_MAX]) */
        {
            int T = omp_get_num_threads();
            int tid = omp_get_thread_num();

            index_t *local = tjmin[tid];

            /* first initialize to ncols */
            for (int w = 1; w <= MERGE_LEVEL_MAX; w++)
                local[w] = mat->ncols;

	    /* compute_jmin takes so little time that it makes no sense
	       optimizing the schedule below */
            #pragma omp for schedule(static)
            for (index_t j = 0; j < mat->ncols; j++) {
                col_weight_t w = mat->wt[j];
                if (0 < w && w <= MERGE_LEVEL_MAX && j < local[w])
                    local[w] = j;
            }

	    /* compute_jmin takes so little time that it makes no sense
	       optimizing the schedule below */
            #pragma omp for schedule(static)
            for (int w = 1; w <= MERGE_LEVEL_MAX; w++) {
                jmin[w] = mat->ncols;
                for (int t = 0; t < T; t++)
                    if (jmin[w] > tjmin[t][w])
                        jmin[w] = tjmin[t][w];
            }
        }
    }

  jmin[0] = 1; /* to tell that jmin was initialized */

  /* make jmin[w] = min(jmin[w'], 1 <= w' <= w) */
  for (int w = 2; w <= MERGE_LEVEL_MAX; w++)
    if (jmin[w - 1] < jmin[w])
      jmin[w] = jmin[w - 1];
}

/* compute column weights (in fact, saturate to cwmax + 1 since we only need to
   know whether the weights are <= cwmax or not) */
static void
compute_weights (filter_matrix_t *mat, index_t *jmin)
{
  double cpu = seconds (), wct = wct_seconds ();
  unsigned char cwmax = mat->cwmax;

  index_t j0;
  if (jmin[0] == 0) /* jmin was not initialized */
    {
      j0 = 0;
      cwmax = MERGE_LEVEL_MAX;
    }
  else
    /* we only need to consider ideals of index >= j0, assuming the weight of
       an ideal cannot decrease (except when decreasing to zero when merged) */
    j0 = jmin[mat->cwmax];

  {
      col_weight_t *Wt[omp_get_max_threads()];
#pragma omp parallel
      {
          int T = omp_get_num_threads();
          int tid = omp_get_thread_num();

          /* we allocate an array of size mat->ncols, but the first j0 entries are unused */
          if (tid == 0)
              Wt[0] = mat->wt; /* trick: we use wt for Wt[0] */
          else
              Wt[tid] = malloc (mat->ncols * sizeof (col_weight_t));
          memset (Wt[tid] + j0, 0, (mat->ncols - j0) * sizeof (col_weight_t));

          /* Thread k accumulates weights in Wt[k].
             We only consider ideals of index >= j0, and put the weight of ideal j,
             j >= j0, in Wt[k][j]. */

          /* using a dynamic or guided schedule here is crucial, since during
	     merge, the distribution of row lengths is no longer uniform
	     (including discarded rows) */
          col_weight_t *Wtk = Wt[tid];
          #pragma omp for schedule(guided)
          for (index_t i = 0; i < mat->nrows; i++) {
              if (mat->rows[i] == NULL) /* row was discarded */
                  continue;
              for (index_t l = matLengthRow (mat, i); l >= 1; l--) {
                  index_t j = matCell (mat, i, l);
                  if (j < j0) /* assume ideals are sorted by increasing order */
                      break;
                  else if (Wtk[j] <= cwmax)      /* (*) HERE */
                      Wtk[j]++;
              }
          }

          /* Thread k accumulates in Wt[0] the weights for the k-th block of columns,
             saturating at cwmax + 1:
             Wt[0][j] = min(cwmax+1, Wt[0][j] + Wt[1][j] + ... + Wt[nthreads-1][j]) */
          col_weight_t *Wt0 = Wt[0];
          #pragma omp for schedule(static) /* slightly better than guided */
          for (index_t i = j0; i < mat->ncols; i++) {
              col_weight_t val = Wt0[i];
              for (int t = 1; t < T; t++)
                  if (val + Wt[t][i] <= cwmax)
                      val += Wt[t][i];
                  else {
                      val = cwmax + 1;
                      break;
                  }
              Wt0[i] = val;
          }

          if (tid > 0)     /* start from 1 since Wt[0] = mat->wt + j0 should be kept */
              free (Wt[tid]);
      }
  }

  if (jmin[0] == 0) /* jmin was not initialized */
    compute_jmin (mat, jmin);

  cpu = seconds () - cpu;
  wct = wct_seconds () - wct;
  print_timings ("   compute_weights took", cpu, wct);
  cpu_t[COMPUTE_W] += cpu;
  wct_t[COMPUTE_W] += wct;
}

/* computes the transposed matrix for columns of weight <= cwmax
   (we only consider columns >= j0) */
static void
compute_R (filter_matrix_t *mat, index_t j0)
{
  double cpu = seconds (), wct = wct_seconds ();

  index_t *Rp = mat->Rp;
  index_t *Rq = mat->Rq;
  index_t *Rqinv = mat->Rqinv;
  uint64_t nrows = mat->nrows;
  uint64_t ncols = mat->ncols;
  int cwmax = mat->cwmax;

  /* compute the number of rows, the indices of the rowd and the row pointers */

  int T = omp_get_max_threads();
  index_t tRnz[T];
  index_t tRn[T];
#pragma omp parallel
  {
          int T = omp_get_num_threads();
          int tid = omp_get_thread_num();
          index_t Rnz = 0;
          index_t Rn = 0;
          #pragma omp for schedule(static) nowait
          for (index_t j = j0; j < ncols; j++) {
              col_weight_t w = mat->wt[j];
              if (0 < w && w <= cwmax) {
                  Rnz += w;
                  Rn++;
              }
          }
          tRnz[tid] = Rnz;
          tRn[tid] = Rn;

#pragma omp barrier

          /* prefix-sum over the T threads (sequentially) */
#pragma omp single
          {
              index_t r = 0;
              index_t s = 0;
              for (int t = 0; t < T; t++) {
                  index_t w = tRnz[t];
                  index_t n = tRn[t];
                  tRnz[t] = r;
                  tRn[t] = s;
                  r += w;
                  s += n;
              }
              mat->Rn = s;
              Rp[s] = r; /* set the last row pointer */
          }
          Rnz = tRnz[tid];
          Rn = tRn[tid];

          #pragma omp for schedule(static) /* static is mandatory here */
          for (index_t j = j0; j < ncols; j++) {
              col_weight_t w = mat->wt[j];
              if (0 < w && w <= cwmax) {
                  Rq[j] = Rn;
                  Rqinv[Rn] = j;
                  Rnz += w;
                  Rp[Rn] = Rnz;
                  Rn++;
              }
          }
  } /* end parallel section */

  index_t Rn = mat->Rn;
  index_t Rnz = Rp[Rn];

  /* allocate variable-sized output (Rp is preallocated) */
  index_t *Ri = malloc_aligned (Rnz * sizeof(index_t), 64);
  mat->Ri = Ri;

  MAYBE_UNUSED double before_extraction = wct_seconds();

  /* dispatch entries */
  #pragma omp parallel for schedule(guided)
  for (index_t i = 0; i < nrows; i++) {
          if (mat->rows[i] == NULL)
                  continue; /* row was discarded */
          for (int k = matLengthRow(mat, i); k >= 1; k--) {
                  index_t j = matCell(mat, i, k);
                  if (j < j0)
                          break;
                  if (mat->wt[j] > cwmax)
                          continue;
                  index_t row = i;
                  index_t col = Rq[j];
                  uint64_t ptr;
                  #pragma omp atomic capture
                  ptr = --Rp[col];
                  Ri[ptr] = row;
          }
  }
  MAYBE_UNUSED double before_compression = wct_seconds();
  MAYBE_UNUSED double end_time = before_compression;

#ifdef BIG_BROTHER
  printf("$$$     compute_R:\n");
  #ifdef BIG_BROTHER_EXPENSIVE
        index_t n_empty = 0;
        for (index_t j = 0; j < ncols; j++)
                if (mat->wt[j] == 0)
                        n_empty++;
        printf("$$$       empty-columns: %d\n", n_empty);
  #endif
  printf("$$$       Rn: % " PRId64 "\n", Rn);
  printf("$$$       Rnz: %" PRId64 "\n", Rnz);
  printf("$$$       timings:\n");
  printf("$$$         row-count: %f\n", before_extraction - wct);
  printf("$$$         extraction: %f\n", before_compression - before_extraction);
  printf("$$$         conversion: %f\n", end_time - before_compression);
  printf("$$$         total: %f\n", end_time - wct);
#endif


  cpu = seconds () - cpu;
  wct = wct_seconds () - wct;
  print_timings ("   compute_R took", cpu, wct);
  cpu_t[COMPUTE_R] += cpu;
  wct_t[COMPUTE_R] += wct;
}


static inline void
decrease_weight (filter_matrix_t *mat, index_t j)
{
  /* only decrease the weight if <= MERGE_LEVEL_MAX,
     since we saturate to MERGE_LEVEL_MAX+1 */
  if (mat->wt[j] <= MERGE_LEVEL_MAX) {
    /* update is enough, we do not need capture since we are not interested
       by the value of wt[j] */
    #pragma omp atomic update
    mat->wt[j]--;
#ifdef BIG_BROTHER_EXPENSIVE
    touched_columns[j] = 1;
#endif
  }
}

static inline void
increase_weight (filter_matrix_t *mat, index_t j)
{
  /* only increase the weight if <= MERGE_LEVEL_MAX,
     since we saturate to MERGE_LEVEL_MAX+1 */
  if (mat->wt[j] <= MERGE_LEVEL_MAX) {
    #pragma omp atomic update
    mat->wt[j]++;
#ifdef BIG_BROTHER_EXPENSIVE
    touched_columns[j] = 1;
#endif
  }
}

/* doit == 0: return the weight of row i1 + row i2
   doit <> 0: add row i2 to row i1.
   New memory is allocated and the old space is freed */
#ifndef FOR_DL
/* special code for factorization */
static void
add_row (filter_matrix_t *mat, index_t i1, index_t i2, MAYBE_UNUSED index_t j)
{
	index_t k1 = matLengthRow(mat, i1);
	index_t k2 = matLengthRow(mat, i2);
	index_t t1 = 1, t2 = 1;
	index_t t = 0;

#ifdef CANCEL
	#pragma omp atomic update
	cancel_rows ++;
#endif

	/* fast-track : don't precompute the size */
	typerow_t *sum = heap_alloc_row(i1, k1 + k2);

	while (t1 <= k1 && t2 <= k2) {
		if (mat->rows[i1][t1] == mat->rows[i2][t2]) {
			decrease_weight(mat, mat->rows[i1][t1]);
			t1 ++, t2 ++;
		} else if (mat->rows[i1][t1] < mat->rows[i2][t2]) {
			sum[++t] = mat->rows[i1][t1++];
		} else {
			increase_weight(mat, mat->rows[i2][t2]);
			sum[++t] = mat->rows[i2][t2++];
		}
	}
	while (t1 <= k1)
	      sum[++t] = mat->rows[i1][t1++];
	while (t2 <= k2) {
	    increase_weight(mat, mat->rows[i2][t2]);
	    sum[++t] = mat->rows[i2][t2++];
	}
	ASSERT(t <= k1 + k2 - 1);

#ifdef CANCEL
	int cancel = (t1 - 1) + (t2 - 1) - (t - 1);
	ASSERT_ALWAYS(cancel < CANCEL_MAX);
	#pragma omp atomic update
	cancel_cols[cancel] ++;
#endif

	heap_resize_last_row(sum, t);
	heap_destroy_row(mat->rows[i1]);
	mat->rows[i1] = sum;
	return;
}
#else /* FOR_DL: j is the ideal to be merged */
#define INT32_MIN_64 (int64_t) INT32_MIN
#define INT32_MAX_64 (int64_t) INT32_MAX

static void
add_row (filter_matrix_t *mat, index_t i1, index_t i2, index_t j)
{
#ifdef CANCEL
	#pragma omp atomic update
	cancel_rows ++;
#endif

  /* first look for the exponents of j in i1 and i2 */
  uint32_t k1 = matLengthRow (mat, i1);
  uint32_t k2 = matLengthRow (mat, i2);
  typerow_t *r1 = mat->rows[i1];
  typerow_t *r2 = mat->rows[i2];
  int32_t e1 = 0, e2 = 0;

  /* search by decreasing ideals as the ideal to be merged is likely large */
  for (int l = k1; l >= 1; l--)
    if (r1[l].id == j) {
	e1 = r1[l].e;
	break;
      }
  for (int l = k2; l >= 1; l--)
    if (r2[l].id == j) {
	e2 = r2[l].e;
	break;
      }

  /* we always check e1 and e2 are not zero, in order to prevent from zero
     exponents that would come from exponent overflows in previous merges */
  ASSERT_ALWAYS (e1 != 0 && e2 != 0);

  int d = (int) gcd_int64 ((int64_t) e1, (int64_t) e2);
  e1 /= -d;
  e2 /= d;
  /* we will multiply row i1 by e2, and row i2 by e1 */

  index_t t1 = 1, t2 = 1, t = 0;

  /* now perform the real merge */
  typerow_t *sum;
  sum = heap_alloc_row(i1, k1 + k2 - 1);

  int64_t e;
  while (t1 <= k1 && t2 <= k2) {
      if (r1[t1].id == r2[t2].id) {
	  /* as above, the exponent e below cannot overflow */
	  e = (int64_t) e2 * (int64_t) r1[t1].e + (int64_t) e1 * (int64_t) r2[t2].e;
	  if (e != 0) { /* exponents do not cancel */
	      ASSERT_ALWAYS(INT32_MIN_64 <= e && e <= INT32_MAX_64);
	      t++;
	      setCell(sum, t, r1[t1].id, e);
	    }
	  else
	    decrease_weight (mat, r1[t1].id);
	  t1 ++, t2 ++;
	}
      else if (r1[t1].id < r2[t2].id)
	{
	  e = (int64_t) e2 * (int64_t) r1[t1].e;
	  ASSERT_ALWAYS(INT32_MIN_64 <= e && e <= INT32_MAX_64);
	  t++;
	  setCell(sum, t, r1[t1].id, e);
	  t1 ++;
	}
      else
	{
	  e = (int64_t) e1 * (int64_t) r2[t2].e;
	  ASSERT_ALWAYS(INT32_MIN_64 <= e && e <= INT32_MAX_64);
	  t++;
	  setCell(sum, t, r2[t2].id, e);
	  increase_weight (mat, r2[t2].id);
	  t2 ++;
	}
    }
  while (t1 <= k1) {
      e = (int64_t) e2 * (int64_t) r1[t1].e;
      ASSERT_ALWAYS(INT32_MIN_64 <= e && e <= INT32_MAX_64);
      t++;
      setCell(sum, t, r1[t1].id, e);
      t1 ++;
    }
  while (t2 <= k2) {
      e = (int64_t) e1 * (int64_t) r2[t2].e;
      ASSERT_ALWAYS(INT32_MIN_64 <= e && e <= INT32_MAX_64);
      t++;
      setCell(sum, t, r2[t2].id, e);
      increase_weight (mat, r2[t2].id);
      t2 ++;
    }
  ASSERT(t <= k1 + k2 - 1);


#ifdef CANCEL
	int cancel = (t1 - 1) + (t2 - 1) - (t - 1);
	ASSERT_ALWAYS(cancel < CANCEL_MAX);
	#pragma omp atomic update
	cancel_cols[cancel] ++;
#endif

  heap_resize_last_row(sum, t);
  heap_destroy_row(mat->rows[i1]);
  mat->rows[i1] = sum;
}
#endif


static void
remove_row (filter_matrix_t *mat, index_t i)
{
  int32_t w = matLengthRow (mat, i);
  for (int k = 1; k <= w; k++)
    decrease_weight (mat, rowCell(mat->rows[i], k));
  heap_destroy_row(mat->rows[i]);
  mat->rows[i] = NULL;
}

#ifdef DEBUG
static void MAYBE_UNUSED
printRow (filter_matrix_t *mat, index_t i)
{
  if (mat->rows[i] == NULL)
    {
      printf ("row %u has been discarded\n", i);
      return;
    }
  int32_t k = matLengthRow (mat, i);
  printf ("%lu [%d]:", (unsigned long) i, k);
  for (int j = 1; j <= k; j++)
#ifndef FOR_DL
    printf (" %lu", (unsigned long) mat->rows[i][j]);
#else
    printf (" %lu^%d", (unsigned long) mat->rows[i][j].id, mat->rows[i][j].e);
#endif
  printf ("\n");
}
#endif

#define BIAS 3

/* classical cost: merge the row of smaller weight with the other ones,
   and return the merge cost (taking account of cancellations).
   id is the index of a row in R. */
static int32_t
merge_cost (filter_matrix_t *mat, index_t id)
{
  index_t lo = mat->Rp[id];
  index_t hi = mat->Rp[id + 1];
  int w = hi - lo;

  if (w == 1)
    return 0; /* ensure all 1-merges are processed before 2-merges with no
		 cancellation */

  if (w > mat->cwmax)
    return INT32_MAX;

  /* find shortest row in the merged column */
  index_t i = mat->Ri[lo];
  int32_t c, cmin = matLengthRow (mat, i);
  for (index_t k = lo + 1; k < hi; k++)
    {
      i = mat->Ri[k];
      c = matLengthRow(mat, i);
      if (c < cmin)
	cmin = c;
    }

  /* fill-in formula for Markowitz pivoting: since w >= 2 we have
     (w - 1) * (cmin - 2) - cmin >= -2, thus adding 3 ensures we
     get a value >= 1, then 1-merges will be merged first */
  return (w - 1) * (cmin - 2) - cmin + BIAS;
}

/* Output a list of merges to a string.
   Assume rep->type = 0.
   size is the length of str.
   Return the number of characters written, except the final \0
   (or that would have been written if that number >= size) */
static int
#ifndef FOR_DL
sreportn (char *str, size_t size, index_signed_t *ind, int n)
#else
sreportn (char *str, size_t size, index_signed_t *ind, int n, index_t j)
#endif
{
  size_t m = 0; /* number of characters written */

  for (int i = 0; i < n; i++)
    {
      m += snprintf (str + m, size - m, "%ld", (long int) ind[i]);
      ASSERT(m < size);
      if (i < n-1)
	{
	  m += snprintf (str + m, size - m, " ");
	  ASSERT(m < size);
	}
    }
#ifdef FOR_DL
  m += snprintf (str + m, size - m, " #%lu", (unsigned long) j);
#endif
  m += snprintf (str + m, size - m, "\n");
  ASSERT(m < size);
  return m;
}

/* Perform the row additions given by the minimal spanning tree (stored in
   history[][]). */
static int
addFatherToSons (index_t history[MERGE_LEVEL_MAX][MERGE_LEVEL_MAX+1],
		 filter_matrix_t *mat, int m, index_t *ind, index_t j,
		 int *father, int *sons)
{
  int i, s, t;

  for (i = m - 2; i >= 0; i--)
    {
      s = father[i];
      t = sons[i];
      if (i == 0)
	{
	  history[i][1] = ind[s];
	  ASSERT(s == 0);
	}
      else
	history[i][1] = -(ind[s] + 1);
      add_row (mat, ind[t], ind[s], j);
      history[i][2] = ind[t];
      history[i][0] = 2;
    }
  return m - 2;
}

/* perform the merge described by the id-th row of R,
   computing the full spanning tree */
static int32_t
merge_do (filter_matrix_t *mat, index_t id, buffer_struct_t *buf)
{
  int32_t c;
  index_t j = mat->Rqinv[id];
  index_t t = mat->Rp[id];
  int w = mat->Rp[id + 1] - t;

  ASSERT (1 <= w && w <= mat->cwmax);

  if (w == 1)
    {
      char s[MERGE_CHAR_MAX];
      int n MAYBE_UNUSED;
      index_signed_t i = mat->Ri[t]; /* only row containing j */
#ifndef FOR_DL
      n = sreportn (s, MERGE_CHAR_MAX, &i, 1);
#else
      n = sreportn (s, MERGE_CHAR_MAX, &i, 1, mat->p[j]);
#endif
      ASSERT(n < MERGE_CHAR_MAX);
      buffer_add (buf, s);
      remove_row (mat, i);
      return -3;
    }

  /* perform the real merge and output to history file */
  index_t *ind = mat->Ri + t;
  char s[MERGE_CHAR_MAX];
  int n = 0; /* number of characters written to s (except final \0) */
  int A[MERGE_LEVEL_MAX][MERGE_LEVEL_MAX];
  fillRowAddMatrix (A, mat, w, ind, j);
  /* mimic MSTWithA */
  int start[MERGE_LEVEL_MAX], end[MERGE_LEVEL_MAX];
  c = minimalSpanningTree (start, end, w, A);
  /* c is the weight of the minimal spanning tree, we have to remove
     the weights of the initial relations */
  for (int k = 0; k < w; k++)
    c -= matLengthRow (mat, ind[k]);
  index_t history[MERGE_LEVEL_MAX][MERGE_LEVEL_MAX+1];
  int hmax = addFatherToSons (history, mat, w, ind, j, start, end);
  for (int i = hmax; i >= 0; i--)
    {
#ifndef FOR_DL
      n += sreportn (s + n, MERGE_CHAR_MAX - n,
		     (index_signed_t*) (history[i]+1), history[i][0]);
#else
      n += sreportn (s + n, MERGE_CHAR_MAX - n,
		     (index_signed_t*) (history[i]+1), history[i][0],
		     mat->p[j]);
#endif
      ASSERT(n < MERGE_CHAR_MAX);
    }
  buffer_add (buf, s);
  remove_row (mat, ind[0]);
  return c;
}

/* accumulate in L all merges of (biased) cost <= cbound.
   L must be preallocated.
   L is a linear array and the merges appear by increasing cost.
   Returns the size of L. */
static int
compute_merges (index_t *L, filter_matrix_t *mat, int cbound)
{
  double cpu = seconds(), wct = wct_seconds();
  index_t Rn = mat->Rn;
  int * cost = malloc(Rn * sizeof(*cost));
  ASSERT_ALWAYS(cost != NULL);
  // int Lp[cbound + 2];  cost pointers

  /* compute the cost of all candidate merges */
  /* A dynamic schedule is needed here, since the columns of larger index have
     smaller weight, thus the load would not be evenly distributed with a
     static schedule. The value 128 was determined optimal experimentally
     on the RSA-512 benchmark with 32 threads, and is better than
     schedule(guided) for RSA-240 with 112 threads. */
  #pragma omp parallel for schedule(dynamic,128)
  for (index_t i = 0; i < Rn; i++)
    cost[i] = merge_cost (mat, i);

  int s;

  
  /* need an array that is visible to all threads in order to do the
   * prefix sum. Wish I knew another way.
   */
  int T = omp_get_max_threads();
  index_t count[T][cbound + 1];

  /* Yet Another Bucket Sort (sigh): sort the candidate merges by cost. Check if worth parallelizing */
#pragma omp parallel
  {
    int tid = omp_get_thread_num();
    index_t *tcount = &count[tid][0];

    memset(tcount, 0, (cbound + 1) * sizeof(index_t));

#pragma omp for schedule(static)  // static is mandatory
    for (index_t i = 0; i < Rn; i++) {
      int c = cost[i];
      if (c <= cbound)
	tcount[c]++;
    }

         
#pragma omp single
    {
      /* prefix-sum */
      s = 0;
      for (int c = 0; c <= cbound; c++) {
	// Lp[c] = s;                     /* global row pointer in L */
	for (int t = 0; t < omp_get_num_threads(); t++) {
	  index_t w = count[t][c];       /* per-thread row pointer in L */
	  count[t][c] = s;
	  s += w;
	}
      }
    }

#pragma omp for schedule(static) // static is mandatory
    for (index_t i = 0; i < Rn; i++) {
      int c = cost[i];
      if (c > cbound)
	continue;
      L[tcount[c]++] = i;
    }
  } /* end parallel section */

  free(cost);

  double end = wct_seconds();
  #ifdef BIG_BROTHER
  	printf("$$$     compute_merges:\n");
  	printf("$$$       candidate-merges: %d\n", s);
  	printf("$$$       timings:\n");
  	printf("$$$         total: %f\n", end - wct);
  #endif
  double cpu2 = seconds() - cpu;
  double wct2 = end - wct;
  print_timings ("   compute_merges took", cpu2, wct2);
  cpu_t[COMPUTE_M] += cpu2;
  wct_t[COMPUTE_M] += wct2;
  return s;
}


/* return the number of merges applied */
static unsigned long
apply_merges (index_t *L, index_t total_merges, filter_matrix_t *mat,
	      buffer_struct_t *Buf)
{
  double cpu3 = seconds (), wct3 = wct_seconds ();
  char * busy_rows = malloc(mat->nrows * sizeof (char));
  memset (busy_rows, 0, mat->nrows * sizeof (char));

  unsigned long nmerges = 0;
  int64_t fill_in = 0;
#ifdef BIG_BROTHER
  unsigned long discarded_early = 0;
  unsigned long discarded_late = 0;
#endif

#ifdef BIG_BROTHER
  #pragma omp parallel reduction(+: fill_in, nmerges, discarded_early, discarded_late)
#else
  #pragma omp parallel reduction(+: fill_in, nmerges)
#endif
  {
    #pragma omp for schedule(guided)
    for (index_t it = 0; it < total_merges; it++) {
      index_t id = L[it];
      index_t lo = mat->Rp[id];
      index_t hi = mat->Rp[id + 1];
      int tid = omp_get_thread_num ();

      /* merge is possible if all its rows are "available" */
      int ok = 1;
      for (index_t k = lo; k < hi; k++) {
	index_t i = mat->Ri[k];
	if (busy_rows[i]) {
	  ok = 0;
#ifdef BIG_BROTHER
          discarded_early++;
#endif
	  break;
	}
      }
      if (ok) {
	  /* check again, since another thread might have reserved a row */
	  for (index_t k = lo; k < hi; k++) {
	    index_t i = mat->Ri[k];
	    char not_ok = 0;
	    /* we could use __sync_bool_compare_and_swap here,
	       but this is more portable and as efficient */
            #if defined(HAVE_OPENMP) && _OPENMP > 201107
	    /* the form of atomic capture below does not seem to be
	       recognized by OpenMP 3.5 (_OPENMP = 201107), see
	       https://cado-nfs-ci.loria.fr/ci/job/future-parallel-merge/job/compile-centos-6-i386/165 */
	    #pragma omp atomic capture
	    #else
	    #pragma omp critical
	    #endif
	    { not_ok = busy_rows[i]; busy_rows[i] = 1; }
	    if (not_ok)
	      {
#ifdef BIG_BROTHER
                discarded_late++;
#endif
		ok = 0;
		break;
	      }
	  }
      }
      if (ok) {
	fill_in += merge_do(mat, id, Buf + tid);
	nmerges ++;
        ASSERT(hi - lo <= MERGE_LEVEL_MAX);
      }
    }  /* for */
  } /* parallel section */

  mat->tot_weight += fill_in;
  /* each merge decreases the number of rows and columns by one */
  mat->rem_nrows -= nmerges;
  mat->rem_ncols -= nmerges;

  double end = wct_seconds();

#ifdef BIG_BROTHER
  printf("$$$     apply-merges:\n");
  printf("$$$       discarded-early: %ld\n", discarded_early);
  printf("$$$       discarded-late: %ld\n", discarded_late);
  printf("$$$       merged: %ld\n", nmerges);
  #ifdef BIG_BROTHER_EXPENSIVE
  	index_t n_rows = 0;
  	for (index_t i = 0; i < mat->nrows; i++)
  	  n_rows += busy_rows[i];
  	printf("$$$       affected-rows: %d\n", n_rows);

  	index_t n_cols = 0;
  	for (index_t j = 0; j < mat->ncols; j++) {
  		n_cols += touched_columns[j];
  		touched_columns[j] = 0;
  	}
	printf("$$$       affected-columns: %d\n", n_cols);
  #endif
  printf("$$$       timings:\n");
  printf("$$$         total: %f\n", end - wct3);
#endif
  free(busy_rows);

  cpu3 = seconds () - cpu3;
  wct3 = end - wct3;
  print_timings ("   apply_merges took", cpu3, wct3);
  cpu_t[APPLY_M] += cpu3;
  wct_t[APPLY_M] += wct3;
  return nmerges;
}

static double
average_density (filter_matrix_t *mat)
{
  return (double) mat->tot_weight / (double) mat->rem_nrows;
}

#ifdef DEBUG
/* duplicate the matrix, where the lines of mat_copy are
   not allocated individually by malloc, but all at once */
static void MAYBE_UNUSED
copy_matrix (filter_matrix_t *mat)
{
  unsigned long weight = mat->tot_weight;
  unsigned long s = weight + mat->nrows;
  index_t *T = malloc (s * sizeof (index_t));
  index_t *p = T;
  double cpu = seconds (), wct = wct_seconds ();
  for (index_t i = 0; i < mat->nrows; i++)
    {
      if (mat->rows[i] == NULL)
	{
	  p[0] = 0;
	  p ++;
	}
      else
	{
	  memcpy (p, mat->rows[i], (mat->rows[i][0] + 1) * sizeof (index_t));
	  p += mat->rows[i][0] + 1;
	}
    }
  print_timings ("   copy_matrix took", seconds () - cpu,
		 wct_seconds () - wct);
  ASSERT_ALWAYS(p == T + s);
  free (T);
}
#endif

#if 0
/* This function outputs the matrix in file 'out' in Sage format:
   M = matrix(...). Then to obtain a figure in Sage:
   sage: %runfile out.sage
   sage: M2 = matrix(RDF,512)
   sage: for i in range(512):
            for j in range(512):
               M2[i,j] = float(log(1+M[i,j]))
   sage: matrix_plot(M2,cmap='Greys')
   sage: matrix_plot(M2,cmap='Greys').save("mat.png")
*/
static void
output_matrix (filter_matrix_t *mat, char *out)
{
#define GREY_SIZE 512
    unsigned long grey[GREY_SIZE][GREY_SIZE];
    unsigned long hi = mat->nrows / GREY_SIZE;
    unsigned long hj = mat->ncols / GREY_SIZE;
    for (int i = 0; i < GREY_SIZE; i++)
      for (int j = 0; j < GREY_SIZE; j++)
	grey[i][j] = 0;
    for (index_t i = 0; i < mat->nrows; i++)
      {
	ASSERT_ALWAYS (mat->rows[i] != NULL);
	index_t ii = i / hi;
	if (ii >= GREY_SIZE)
	  ii = GREY_SIZE - 1;
	for (unsigned int k = 1; k <= matLengthRow(mat, i); k++)
	  {
	    index_t j = matCell(mat, i, k);
	    index_t jj = j / hj;
	    if (jj >= GREY_SIZE)
	      jj = GREY_SIZE - 1;
	    grey[ii][jj] ++;
	  }
      }
    FILE *fp = fopen (out, "w");
    fprintf (fp, "M=matrix([");
    for (int i = 0; i < GREY_SIZE; i++)
      {
	fprintf (fp, "[");
	int k = i;
	for (int j = 0; j < GREY_SIZE; j++)
	  {
	    fprintf (fp, "%lu", grey[k][j]);
	    if (j + 1 < GREY_SIZE)
	      fprintf (fp, ",");
	  }
	fprintf (fp, "]");
	if (i + 1 < GREY_SIZE)
	  fprintf (fp, ",");
      }
    fprintf (fp, "])\n");
    fclose (fp);
}
#endif

int
main (int argc, char *argv[])
{
    char *argv0 = argv[0];

    filter_matrix_t mat[1];
    report_t rep[1];

    int nthreads = 1;
    uint32_t skip = DEFAULT_MERGE_SKIP;
    double target_density = DEFAULT_MERGE_TARGET_DENSITY;

#ifdef HAVE_MINGW
    _fmode = _O_BINARY;     /* Binary open for all files */
#endif

    double tt;
    double cpu0 = seconds ();
    double wct0 = wct_seconds ();
    param_list pl;
    param_list_init (pl);
    declare_usage(pl);
    argv++,argc--;

    param_list_configure_switch (pl, "-v", &verbose);
    param_list_configure_switch(pl, "force-posix-threads", &filter_rels_force_posix_threads);

#ifdef HAVE_MINGW
    _fmode = _O_BINARY;     /* Binary open for all files */
#endif

    if (argc == 0)
      usage (pl, argv0);

    for( ; argc ; ) {
      if (param_list_update_cmdline(pl, &argc, &argv)) continue;
      fprintf (stderr, "Unknown option: %s\n", argv[0]);
      usage (pl, argv0);
    }
    /* print command-line arguments */
    verbose_interpret_parameters (pl);
    param_list_print_command_line (stdout, pl);
    fflush(stdout);

    const char *purgedname = param_list_lookup_string (pl, "mat");
    const char *outname = param_list_lookup_string (pl, "out");
    const char *path_antebuffer = param_list_lookup_string(pl, "path_antebuffer");

    param_list_parse_int (pl, "t", &nthreads);
#ifdef HAVE_OPENMP
    omp_set_num_threads (nthreads);
#endif

    param_list_parse_uint (pl, "skip", &skip);

    param_list_parse_double (pl, "target_density", &target_density);

    /* Some checks on command line arguments */
    if (param_list_warn_unused(pl))
    {
      fprintf(stderr, "Error, unused parameters are given\n");
      usage(pl, argv0);
    }

    if (purgedname == NULL)
    {
      fprintf(stderr, "Error, missing -mat command line argument\n");
      usage (pl, argv0);
    }
    if (outname == NULL)
    {
      fprintf(stderr, "Error, missing -out command line argument\n");
      usage (pl, argv0);
    }

    heap_setup();
    set_antebuffer_path (argv0, path_antebuffer);

    /* Read number of rows and cols on first line of purged file */
    purgedfile_read_firstline (purgedname, &(mat->nrows), &(mat->ncols));

#if (SIZEOF_INDEX == 4)
    if (mat->nrows >> 32)
      {
	fprintf (stderr, "Error, nrows = %" PRIu64 " larger than 2^32, please recompile with -DSIZEOF_INDEX=8\n", mat->nrows);
	exit (EXIT_FAILURE);
      }
    if (mat->ncols >> 32)
      {
	fprintf (stderr, "Error, ncols = %" PRIu64 " larger than 2^32, please recompile with -DSIZEOF_INDEX=8\n", mat->ncols);
	exit (EXIT_FAILURE);
      }
#endif

    /* initialize rep (i.e., mostly opens outname) and write matrix dimension */
    rep->type = 0;
    rep->outfile = fopen_maybe_compressed (outname, "w");
    ASSERT_ALWAYS(rep->outfile != NULL);

    /* some explanation about the history file */
    fprintf (rep->outfile, "# Every line starting with # is ignored.\n");
    fprintf (rep->outfile, "# A line i1 i2 ... ik means that row i1 ");
    fprintf (rep->outfile, "is added to i2, ..., ik, and row i1\n");
    fprintf (rep->outfile, "# is removed afterwards ");
    fprintf (rep->outfile, "(where row 0 is the first line in *.purged.gz).\n");
#ifdef FOR_DL
    fprintf (rep->outfile, "# A line ending with #j ");
    fprintf (rep->outfile, "means that ideal of index j should be merged.\n");
#endif

    /* initialize the matrix structure */
    initMat (mat, skip);

    /* we bury the 'skip' ideals of smallest index */
    mat->skip = skip;


    /* Read all rels and fill-in the mat structure */
    tt = seconds ();
    filter_matrix_read (mat, purgedname);
    printf ("Time for filter_matrix_read: %2.2lfs\n", seconds () - tt);

    buffer_struct_t *Buf = buffer_init (nthreads);

    double cpu_after_read = seconds ();
    double wct_after_read = wct_seconds ();

    /* jmin[w] for 1 <= w <= MERGE_LEVEL_MAX is the smallest column of weight w
       at beginning. We set jmin[0] to 0 to tell that jmin[] was not
       initialized. */
    index_t jmin[MERGE_LEVEL_MAX + 1] = {0,};

    recompress (mat, jmin);

    // output_matrix (mat, "out.sage");

    /* Allocate the transposed matrix R in CSR format. Since Rp is of fixed
       size, we allocate it for once. However, the size of Ri will vary from
       step to step. */
    mat->Rp = malloc ((mat->ncols + 1) * sizeof (index_t));
    mat->Ri = NULL;
    mat->Rq = malloc (mat->ncols * sizeof (index_t));
    mat->Rqinv = malloc (mat->ncols * sizeof (index_t));

#ifdef BIG_BROTHER
    touched_columns = malloc(mat->ncols * sizeof(*touched_columns));
    memset(touched_columns, 0, mat->ncols * sizeof(*touched_columns));
#endif

    printf ("Using MERGE_LEVEL_MAX=%d, CBOUND_INCR=%d",
	    MERGE_LEVEL_MAX, CBOUND_INCR);
#ifdef USE_ARENAS
    printf (", M_ARENA_MAX=%d", arenas);
#endif
    printf (", PAGE_SIZE=%d", PAGE_SIZE);
#ifdef HAVE_OPENMP
    /* https://stackoverflow.com/questions/38281448/how-to-check-the-version-of-openmp-on-windows
       201511 is OpenMP 4.5 */
    printf (", OpenMP %d", _OPENMP);
#endif
    printf ("\n");

    printf ("N=%" PRIu64 " W=%" PRIu64 " W/N=%.2f cpu=%.1fs wct=%.1fs mem=%luM\n",
	    mat->rem_nrows, mat->tot_weight, average_density (mat),
	    seconds () - cpu0, wct_seconds () - wct0,
	    PeakMemusage () >> 10);
#ifdef BIG_BROTHER
    printf("$$$ N: %" PRId64 "\n", mat->nrows);
    printf("$$$ start:\n");
#endif

    fflush (stdout);

    mat->cwmax = 2;


    // copy_matrix (mat);

#if defined(DEBUG) && defined(FOR_DL)
    /* compute the minimum/maximum coefficients */
    int32_t min_exp = 0, max_exp = 0;
    for (index_t i = 0; i < mat->nrows; i++)
      if (mat->rows[i] != NULL)
	{
	  ideal_merge_t *ri = mat->rows[i];
	  for (unsigned int k = 1; k <= matLengthRow (mat, i); k++)
	    {
	      int32_t e = ri[k].e;
	      if (e < min_exp)
		min_exp = e;
	      if (e > max_exp)
		max_exp = e;
	    }
	}
    printf ("min_exp=%d max_exp=%d\n", min_exp, max_exp);
#endif

    unsigned long lastN, lastW;
    double lastWoverN;
    int cbound = BIAS; /* bound for the (biased) cost of merges to apply */

    /****** begin main loop ******/
    while (1) {
	double cpu1 = seconds (), wct1 = wct_seconds ();
	pass++;

        if (pass == 2 || mat->cwmax > 2)
                full_garbage_collection(mat);

	/* Once cwmax >= 3, tt each pass, we increase cbound to allow more
	   merges. If one decreases CBOUND_INCR, the final matrix will be
	   smaller, but merge will take more time.
	   If one increases CBOUND_INCR, merge will be faster, but the final
	   matrix will be larger. */
	if (mat->cwmax > 2)
		cbound += CBOUND_INCR;

	lastN = mat->rem_nrows;
	lastW = mat->tot_weight;
	lastWoverN = (double) lastW / (double) lastN;

	#ifdef TRACE_J
	for (index_t i = 0; i < mat->ncols; i++) {
		if (mat->rows[i] == NULL)
			continue;
		for (index_t k = 1; k <= matLengthRow(mat, i); k++)
			if (mat->rows[i][k] == TRACE_J)
		printf ("ideal %d in row %lu\n", TRACE_J, (unsigned long) i);
	}
	#endif

	#ifdef BIG_BROTHER
		printf("$$$   - pass: %d\n", pass);
		printf("$$$     cwmax: %d\n", mat->cwmax);
		printf("$$$     cbound: %d\n", cbound);
	#endif

	/* we only compute the weights at pass 1, afterwards they will be
	   updated at each merge */
	if (pass == 1)
		compute_weights (mat, jmin);

	compute_R (mat, jmin[mat->cwmax]);

	index_t *L = malloc(mat->Rn * sizeof(index_t));
	index_t n_possible_merges = compute_merges(L, mat, cbound);

	unsigned long nmerges = apply_merges(L, n_possible_merges, mat, Buf);
	buffer_flush (Buf, nthreads, rep->outfile);
	free(L);

	free_aligned (mat->Ri);

	/* settings for next pass */
  	if (mat->cwmax == 2) { /* we first process all 2-merges */
		if (nmerges == n_possible_merges)
			mat->cwmax++;
	} else {
		if (mat->cwmax < MERGE_LEVEL_MAX)
			mat->cwmax ++;
	}

	if (mat->rem_ncols < 0.66 * mat->ncols) {
	  static int pass = 0;
	  printf("============== Recompress %d ==============\n", ++pass);
	  recompress(mat, jmin);
	}

	cpu1 = seconds () - cpu1;
	wct1 = wct_seconds () - wct1;
	print_timings ("   pass took", cpu1, wct1);
	cpu_t[PASS] += cpu1;
	wct_t[PASS] += wct1;

	#ifdef BIG_BROTHER
	    printf("$$$     timings:\n");
	    printf("$$$       total: %f\n", wct1);
	#endif

	/* estimate current average fill-in */
	double av_fill_in = ((double) mat->tot_weight - (double) lastW)
	  / (double) (lastN - mat->rem_nrows);

	printf ("N=%" PRIu64 " W=%" PRIu64 " (%.0fMB) W/N=%.2f fill-in=%.2f cpu=%.1fs wct=%.1fs mem=%luM [pass=%d,cwmax=%d]\n",
		mat->rem_nrows, mat->tot_weight,
		9.5367431640625e-07 * (mat->rem_nrows + mat->tot_weight) * sizeof(index_t),
		(double) mat->tot_weight / (double) mat->rem_nrows, av_fill_in,
		seconds () - cpu0, wct_seconds () - wct0,
		PeakMemusage () >> 10, pass, mat->cwmax);
	fflush (stdout);

	if (average_density (mat) >= target_density)
		break;

	if (nmerges == 0 && mat->cwmax == MERGE_LEVEL_MAX)
		break;
    }
    /****** end main loop ******/
    pass++;

#if defined(DEBUG) && defined(FOR_DL)
    min_exp = 0; max_exp = 0;
    for (index_t i = 0; i < mat->nrows; i++)
      if (mat->rows[i] != NULL)
	{
	  for (unsigned int k = 1; k <= matLengthRow (mat, i); k++)
	    {
	      int32_t e = mat->rows[i][k].e;
	      if (e < min_exp)
		min_exp = e;
	      if (e > max_exp)
		max_exp = e;
	    }
	}
    printf ("min_exp=%d max_exp=%d\n", min_exp, max_exp);
#endif

    fclose_maybe_compressed (rep->outfile, outname);

    if (average_density (mat) > target_density)
      {
	/* estimate N for W/N = target_density, assuming W/N = a*N + b */
	unsigned long N = mat->rem_nrows;
	double WoverN = (double) mat->tot_weight / (double) N;
	double a = (lastWoverN - WoverN) / (double) (lastN - N);
	double b = WoverN - a * (double) N;
	/* we want target_density = a*N_target + b */
	printf ("Estimated N=%" PRIu64 " for W/N=%.2f\n",
		(uint64_t) ((target_density - b) / a), target_density);
      }

    print_timings ("compute_weights:", cpu_t[COMPUTE_W], wct_t[COMPUTE_W]);
    print_timings ("compute_R      :", cpu_t[COMPUTE_R], wct_t[COMPUTE_R]);
    print_timings ("compute_merges :", cpu_t[COMPUTE_M], wct_t[COMPUTE_M]);
    print_timings ("apply_merges   :", cpu_t[APPLY_M], wct_t[APPLY_M]);
    print_timings ("pass           :", cpu_t[PASS], wct_t[PASS]);
    print_timings ("recompress     :", cpu_t[RECOMPRESS], wct_t[RECOMPRESS]);
    print_timings ("buffer_flush   :", cpu_t[FLUSH], wct_t[FLUSH]);
    print_timings ("garbage_coll   :", cpu_t[GC], wct_t[GC]);

    printf ("Final matrix has N=%" PRIu64 " nc=%" PRIu64 " (%" PRIu64
	    ") W=%" PRIu64 "\n", mat->rem_nrows, mat->rem_ncols,
	    mat->rem_nrows - mat->rem_ncols, mat->tot_weight);
    fflush (stdout);

    printf ("Before cleaning memory:\n");
    print_timing_and_memory (stdout, cpu_after_read, wct_after_read);

    buffer_clear (Buf, nthreads);

    heap_clear ();

#ifdef FOR_DL
    free (mat->p);
#endif
    free (mat->Rp);
    free (mat->Rq);
    free (mat->Rqinv);

    clearMat (mat);

    param_list_clear (pl);

    printf ("After cleaning memory:\n");
    print_timing_and_memory (stdout, cpu_after_read, wct_after_read);

    /* print total time and memory (including reading the input matrix,
       initializing and free-ing all data) */
    print_timing_and_memory (stdout, cpu0, wct0);

#ifdef CANCEL
    unsigned long tot_cancel = 0;
    printf ("cancel_rows=%lu\n", cancel_rows);
    for (int i = 0; i < CANCEL_MAX; i++)
      if (cancel_cols[i] != 0)
	{
	  tot_cancel += cancel_cols[i] * i;
	  printf ("cancel_cols[%d]=%lu\n", i, cancel_cols[i]);
	}
    printf ("tot_cancel=%lu\n", tot_cancel);
#endif

    return 0;
}
