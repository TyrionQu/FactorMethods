/*
  Polynomial selection using Kleinjung's algorithm (cf slides presented
  at the CADO Workshop in October 2008, Nancy, France).

  [1. Run and parameters]

  The parameters are similar to those in polyselect2.c, except the following,

  "-nq xxx" denotes the number of special-q's trials for each ad;

  Please report bugs to the Bug Tracking System on:
  https://gforge.inria.fr/tracker/?atid=7442&group_id=2065&func=browse
*/

#define EMIT_ADDRESSABLE_shash_add

#include "cado.h" // IWYU pragma: keep
/* The following avoids to put #ifdef HAVE_OPENMP ... #endif around each
 * OpenMP pragma. It should come after cado.h, which sets -Werror=all.
 *
#ifdef  __GNUC__
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
 *
 * unfortunately, while it looks like a reasonable thing to do in theory,
 * it's gcc specific. We can't expect such a thing to work with other
 * compilers.
 */
#include <stdbool.h>    // bool
#include <stdio.h>
#include <stdlib.h>     // malloc ...
#include <stdint.h>     // uint64_t
#include <limits.h> /* for CHAR_BIT */
#include <float.h> // DBL_MAX
#include <math.h> // sqrt
#include <gmp.h>
#include "omp_proxy.h"
#include "gcd.h"       // for gcd_ul
#include "getprime.h"   // getprime
#include "gmp_aux.h"       // mpz_set_uint64
#include "mpz_poly.h"
#include "roots_mod.h"
#include "size_optimization.h"
#include "timing.h"             // for seconds
#include "usp.h"        // usp_root_data
#include "verbose.h"             // verbose_output_print
#include "portability.h"        // lrand48      // IWYU pragma: keep
#include "cado_poly.h"
#include "auxiliary.h"
#include "polyselect_str.h"
#include "polyselect_arith.h"
#include "modredc_ul.h"
#include "macros.h" // ASSERT
#include "params.h"

#define INIT_FACTOR 8UL
#define PREFIX_HASH
//#define DEBUG_POLYSELECT

#ifdef PREFIX_HASH
char *phash = "# ";
#else
char *phash = "";
#endif

#define BATCH_SIZE 20    /* number of special (q, r) per batch */
#define KEEP 10          /* number of best raw polynomials kept */
#define DEFAULT_NQ 1000  /* default max num of nq considered for each ad */
/* Read-Only */
uint32_t *Primes = NULL;
unsigned long lenPrimes = 1; // length of Primes[]
unsigned long nq = DEFAULT_NQ;
size_t keep = KEEP;
const double exp_rot[] = {0, 0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 0};
static int verbose = 0;
unsigned int sopt_effort = SOPT_DEFAULT_EFFORT; /* size optimization effort */
static unsigned long incr = DEFAULT_INCR;
cado_poly best_poly, curr_poly;
double best_E = 0.0; /* Murphy's E (the larger the better) */
double maxtime = DBL_MAX;

/* read-write global variables */
int tot_found = 0; /* total number of polynomials */
unsigned long opt_found = 0; /* number of size-optimized polynomials */
double potential_collisions = 0.0, aver_raw_lognorm = 0.0,
  var_raw_lognorm = 0.0;
#define LOGNORM_MAX 999.99
double min_raw_lognorm = LOGNORM_MAX, max_raw_lognorm = 0.0;
data_t data_opt_lognorm, data_exp_E, data_beta, data_eta, data_best_exp_E;
data_t raw_proj_alpha, opt_proj_alpha;
unsigned long collisions = 0;
unsigned long discarded1 = 0; /* f[d] * f[d-2] > 0 */
unsigned long discarded2 = 0; /* f[d-1] * f[d-3] > 0 */
unsigned long collisions_good = 0;
double *best_opt_logmu, *best_exp_E;
double optimize_time = 0.0;
mpz_t admin, admax;
double target_E = 0.0; /* target E-value, 0.0 if not given */

/* inline function */
extern void shash_add (shash_t, uint64_t);

/* -- functions starts here -- */

/* crt, set r and qqz */
/* this routine is called from polyselect_arith.c and twocubics.c */
void
crt_sq ( mpz_t qqz,
         mpz_t r,
         unsigned long *q,
         unsigned long *rq,
         unsigned long lq )
{
  mpz_t prod, pprod, mod, inv, sum;
  unsigned long i;
  unsigned long qq[lq];

  mpz_init_set_ui (prod, 1);
  mpz_init (pprod);
  mpz_init (mod);
  mpz_init (inv);
  mpz_init_set_ui (sum, 0);

  for (i = 0; i < lq; i ++) {
    qq[i] = q[i] * q[i]; // q small
    mpz_mul_ui (prod, prod, qq[i]);
  }

  for (i = 0; i < lq; i ++) {
    mpz_divexact_ui (pprod, prod, qq[i]);
    mpz_set_ui (mod, qq[i]);
    mpz_invert (inv, pprod, mod);
    mpz_mul_ui (inv, inv, rq[i]);
    mpz_mul (inv, inv, pprod);
    mpz_add (sum, sum, inv);
  }

  mpz_mod (sum, sum, prod);
  mpz_set (r, sum);
  mpz_set (qqz, prod);

  mpz_clear (prod);
  mpz_clear (pprod);
  mpz_clear (mod);
  mpz_clear (inv);
  mpz_clear (sum);
}

/* check that l <= m0/P^2 where l = p1 * p2 * q with P <= p1, p2 <= 2P
   and q is the product of special-q primes.
   This will ensure that we can do rotation by x^(d-3)*g(x), since the
   expected value of a[d-2] is m0/P^2, and x^(d-3)*g(x) has coefficient
   l for degree d-2. */
static int
check_parameters (mpz_t m0, double q)
{
  return pow ((double) Primes[lenPrimes - 1], 4.0) * q < mpz_get_d (m0);
}

/* given a distribution with mean m and variance v, estimate the parameters
   beta and eta from a matching Weibull distribution, using the method of
   moments:
   m = eta * gamma (1 + 1/beta)
   v = eta^2 * [gamma (1 + 2/beta) - gamma (1 + 1/beta)^2] */
static void
estimate_weibull_moments (double *beta, double *eta, data_t s)
{
  double m = data_mean (s);
  double v = data_var (s);
  double y = sqrt (v) / m;

  y = y * (0.7796968012336761 + y * (0.61970313728462 + 0.0562963108244 * y));
  *beta = 1.0 / y;
  *eta = m * (1.0 + y * (0.57721566490153 - 0.655878071520 * y));
}

/* Estimation via extreme values: we cut the total n values into samples of k
   values, and for each sample we keep only the minimum. If the series of
   minimum values satisfies a Weilbull distribution with parameters beta and eta,
   then the original one has parameters beta (identical) and eta*k^(1/beta).
   Here we choose k near sqrt(n). */
static void
estimate_weibull_moments2 (double *beta, double *eta, data_t s)
{
  unsigned long n = s->size;
  unsigned long i, j, k, p, u;
  data_t smin;
  double min, eta_min;

  ASSERT_ALWAYS(n > 0);

  data_init (smin);

  k = (unsigned long) sqrt ((double) n); /* sample size */
  /* We consider full samples only. Since we call this function several times
     with the same sequence, we perform a random permutation of the sequence
     at each call to avoid side effects due to the particular order of
     elements. In practice instead of considering s[j] we consider
     s[(p*j) % n] where p is random with gcd(p,n)=1. */
  do
    p = lrand48 () % n;
  while (gcd_uint64 (p, n) != 1);
  for (i = 0; i + k <= n; i += k)
    {
      for (j = i, min = DBL_MAX; j < i + k; j++)
        {
	  u = (p * j) % n;
          if (s->x[u] < min)
            min = s->x[u];
        }
      data_add (smin, min);
    }
  estimate_weibull_moments (beta, &eta_min, smin);
  data_clear (smin);
  *eta = eta_min * pow ((double) k, 1.0 / *beta);
}

static double
get_ad_double (unsigned long idx)
{
  return (double) mpz_get_d (admin) + (double) incr * (double) idx;
}

/* print poly info */
static void
print_poly_info ( char *buf,
                  size_t size,
                  mpz_t *f,
                  const unsigned int d,
                  mpz_t g[2],
                  const mpz_t n,
                  const int raw,
                  const char *prefix,
                  bool raw_option,
                  unsigned long idx)
{
  unsigned int i, nroots;
  double skew, logmu, exp_E;
  mpz_poly F, G;
  F->coeff = f;
  F->deg = d;
  G->coeff = g;
  G->deg = 1;
  size_t np = 0;

  if (raw_option)
    {
      np += snprintf (buf + np, size - np, "# Raw polynomial:\n");
#ifdef HAVE_OPENMP
#pragma omp critical
#endif
      data_add (raw_proj_alpha, get_alpha_projective (F, get_alpha_bound ()));
    }
  else
    {
      snprintf (buf + np, size - np, "# Size-optimized polynomial:\n");
#ifdef HAVE_OPENMP
#pragma	omp critical
#endif
      data_add (opt_proj_alpha, get_alpha_projective (F, get_alpha_bound ()));
    }

  np += gmp_snprintf (buf + np, size - np, "%sn: %Zd\n", prefix, n);
  np += gmp_snprintf (buf + np, size - np, "%sY1: %Zd\n%sY0: %Zd\n", prefix, g[1], prefix, g[0]);
  for (i = d + 1; i -- != 0; )
    np += gmp_snprintf (buf + np, size - np, "%sc%u: %Zd\n", prefix, i, f[i]);
  skew = L2_skewness (F, SKEWNESS_DEFAULT_PREC);
  nroots = numberOfRealRoots (f, d, 0, 0, NULL);
  logmu = L2_lognorm (F, skew);
  exp_E = logmu + expected_rotation_gain (F, G);
  if (raw == 1)
    np += snprintf (buf + np, size - np, "# raw exp_E");
  else
    np += snprintf (buf + np, size - np, "# exp_E");

  np += snprintf (buf + np, size - np, " %1.2f, lognorm %1.2f, skew %1.2f, %u rroots\n",
           exp_E, logmu, skew, nroots);

  if (!raw_option && (target_E != 0.0 || maxtime < DBL_MAX))
    {
      double beta, eta, prob;

      /* estimate the parameters of a Weibull distribution for E */
      estimate_weibull_moments2 (&beta, &eta, data_exp_E);

      if (target_E != 0.0)
	{
	  prob = 1.0 - exp (- pow (target_E / eta, beta));
	  if (prob == 0) /* for x small, exp(x) ~ 1+x */
	    prob = pow (target_E / eta, beta);
	  np += snprintf (buf + np, size - np,
		  "# E: %lu, min %.2f, avg %.2f, max %.2f, stddev %.2f\n",
		  data_exp_E->size, data_exp_E->min, data_mean (data_exp_E),
		  data_exp_E->max, sqrt (data_var (data_exp_E)));
	  np += snprintf (buf + np, size - np,
		  "# target_E=%.2f: collisions=%.2e, time=%.2e"
		  " (beta %.2f,eta %.2f)\n",
		  target_E, 1.0 / prob, seconds () / (prob * collisions_good),
		  beta, eta);
	}
      else /* maxtime < DBL_MAX */
	{
	  /* time = seconds () / (prob * collisions_good)
	     where  prob = 1 - exp (-(E/eta)^beta) */
	  unsigned long n = collisions_good; /* #polynomials found so far */
	  double time_so_far = seconds ();
	  double time_per_poly = time_so_far / n; /* average time per poly */
	  double admin_d = mpz_get_d (admin);
	  double ad_d = get_ad_double (idx);
	  double adrange = (ad_d - admin_d) * (maxtime / time_so_far);
	  adrange = 2.00e+15 - 99900000000000.0;
	  prob = time_so_far / (maxtime * n);
	  double E = eta * pow (-log (1 - prob), 1.0 / beta);
#ifdef HAVE_OPENMP
#pragma omp critical
#endif
	  data_add (data_best_exp_E, E);
	  /* since the values of (eta,beta) fluctuate a lot, because
	     they depend on the random samples in estimate_weibull_moments2,
	     we take the average value for best_exp_E */
	  E = data_mean (data_best_exp_E);
	  np += snprintf (buf + np, size - np,
			  "# %.2fs/poly, eta %.2f, beta %.3f, admax %.2e, best exp_E %.2f\n",
			  time_per_poly, eta, beta, admin_d + adrange, E);
	}
    }

  if (!raw_option)
    np += snprintf (buf + np, size - np, "\n");
  ASSERT_ALWAYS(np < size);
}


/* the number of expected collisions is 8*lenPrimes^2/2/(2P)^2 */
static double
expected_collisions (uint32_t twoP)
{
  double m = (lenPrimes << 1) / (double) twoP;
  /* we multiply by 0.5 because we discard collisions with f[d] * f[d-2] > 0 */
  return 0.5 * m * m;
}

static void
check_divexact_ui(mpz_t r, const mpz_t d, const char *d_name MAYBE_UNUSED,
                  const unsigned long q, const char *q_name MAYBE_UNUSED)
{
#ifdef DEBUG_POLYSELECT
  if (mpz_divisible_ui_p (d, q) == 0)
  {
    gmp_fprintf (stderr, "Error: %s=%Zd not divisible by %s=%lu\n",
                 d_name, d, q_name, q);
    exit (1);
  }
#endif
  mpz_divexact_ui (r, d, q);
}

static void
check_divexact(mpz_t r, const mpz_t d, const char *d_name MAYBE_UNUSED, const mpz_t q,
               const char *q_name MAYBE_UNUSED)
{
#ifdef DEBUG_POLYSELECT
  if (mpz_divisible_p (d, q) == 0)
  {
    gmp_fprintf (stderr, "Error: %s=%Zd not divisible by %s=%Zd\n",
                 d_name, d, q_name, q);
    exit (1);
  }
#endif
  mpz_divexact (r, d, q);
}

/* idx is the index of the raw (non-optimized) polynomial,
   with ad = admin + idx * incr */
static void
output_polynomials (mpz_t *fold, const unsigned long d, mpz_t *gold,
                    const mpz_t N, mpz_t *f, mpz_t *g, unsigned long idx)
{
  size_t sz = mpz_sizeinbase (N, 10);
  int length = sz*12;
  char *str_old = malloc(length);
  char *str = malloc(length);
  if (fold != NULL && gold != NULL) {
    if (str_old != NULL)
      print_poly_info (str_old, length, fold, d, gold, N, 1, phash, 1, idx);
  }
  if (str != NULL)
    print_poly_info (str, length, f, d, g, N, 0, "", 0, idx);

#ifdef HAVE_OPENMP
#pragma omp critical
#endif
  {
    if (fold != NULL && gold != NULL)
      if (str_old != NULL)
        printf("%s",str_old);
    if (str != NULL)
      printf("%s",str);
    fflush (stdout);
  }

  if (str_old != NULL)
    free (str_old);
  if (str != NULL)
    free (str);
}

/* Insert a value into a sorted array of length len.
   Returns 1 if element was inserted, 0 if it was too big */
static int
sorted_insert_double(double *array, const size_t len, const double value)
{
  size_t k;
  int result = 0;
  if (len == 0)
    return 0;
#ifdef HAVE_OPENMP
#pragma omp critical
#endif
  if (value < array[len - 1]) {
    for (k = len - 1; k > 0 && value < array[k-1]; k--)
      array[k] = array[k-1];
    array[k] = value;
    result = 1;
  }
  return result;
}

/* return 1 if the polynomial is ok and among the best ones,
   otherwise return 0 */
static int
optimize_raw_poly (mpz_poly F, mpz_t *g)
{
  double skew;
  mpz_t t;
  double st, logmu, exp_E;

  /* check that the algebraic polynomial has content 1, otherwise skip it */
  mpz_init (t);
  mpz_poly_content (t, F);
  if (mpz_cmp_ui (t, 1) != 0)
    {
      mpz_clear (t);
      return 0;
    }
  mpz_clear (t);

  /* optimize size */
  mpz_poly G;
  G->deg = 1;
  G->alloc = 2;
  G->coeff = g;

  st = seconds_thread ();
  size_optimization (F, G, F, G, sopt_effort, verbose);
  st = seconds_thread () - st;
#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
  optimize_time += st;
#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
  opt_found ++;

  /* polynomials with f[d-1] * f[d-3] > 0 *after* size-optimization
     give worse exp_E values */
  int d = F->deg;
  if (mpz_sgn (F->coeff[d-1]) * mpz_sgn (F->coeff[d-3]) > 0)
    {
#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
      discarded2 ++;
      return 0;
    }

  skew = L2_skewness (F, SKEWNESS_DEFAULT_PREC);
  logmu = L2_lognorm (F, skew);
  /* expected_rotation_gain() takes into account the projective alpha */
  exp_E = logmu + expected_rotation_gain ((mpz_poly_ptr) F, (mpz_poly_ptr) G);

  sorted_insert_double (best_opt_logmu, keep, logmu);
  sorted_insert_double (best_exp_E, keep, exp_E);

#ifdef HAVE_OPENMP
#pragma omp critical
#endif
  {
    collisions_good ++;
    data_add (data_opt_lognorm, logmu);
    data_add (data_exp_E, exp_E);
  }

  return 1;
}

static unsigned long
get_idx (mpz_t ad)
{
  mpz_t t;
  unsigned long idx;
  mpz_init_set (t, ad);
  mpz_sub (t, t, admin);
  mpz_divexact_ui (t, t, incr);
  idx = mpz_get_ui (t);
  mpz_clear (t);
  return idx;
}

/* rq is a root of N = (m0 + rq)^d mod (q^2) */
/* this routine is called from polyselect_str.c */
void
match (unsigned long p1, unsigned long p2, const int64_t i, mpz_t m0,
       mpz_t ad, unsigned long d, mpz_t N, uint64_t q, mpz_t rq)
{
  mpz_t l, mtilde, m, adm1, t, k, *f, g[2], *fold, gold[2];
  int cmp, did_optimize;
  double skew, logmu;
  mpz_poly F;

  /* the expected rotation space is S^5 for degree 6 */
#ifdef DEBUG_POLYSELECT
  gmp_printf ("Found match: (%lu,%lld) (%lu,%lld) for "
	      "ad=%Zd, q=%llu, rq=%Zd\n",
              p1, (long long) i, p2, (long long) i, ad,
              (unsigned long long) q, rq);
  gmp_printf ("m0=%Zd\n", m0);
#endif

  mpz_init (l);
  mpz_init (m);
  mpz_init (t);
  mpz_init (k);
  mpz_init (adm1);
  mpz_init (mtilde);
  mpz_init (g[0]);
  mpz_init (g[1]);
  mpz_init (gold[0]);
  mpz_init (gold[1]);
  mpz_poly_init (F, d);
  F->deg = d;
  f = F->coeff;
  fold = (mpz_t*) malloc ((d + 1) * sizeof (mpz_t));
  if (fold == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in match\n");
    exit (1);
  }
  for (unsigned long j = 0; j <= d; j++)
    mpz_init (fold[j]);
  /* we have l = p1*p2*q */
  mpz_set_ui (l, p1);
  mpz_mul_ui (l, l, p2);
  mpz_mul_ui (l, l, q);
  /* mtilde = m0 + rq + i*q^2 */
  mpz_set_si (mtilde, i);
  mpz_mul_ui (mtilde, mtilde, q);
  mpz_mul_ui (mtilde, mtilde, q);
  mpz_add (mtilde, mtilde, rq);
  mpz_add (mtilde, mtilde, m0);
  /* we should have Ntilde - mtilde^d = 0 mod {p1^2,p2^2,q^2} */

  /* Small improvement: we have Ntilde = mtilde^d + l^2*R with R small.
     If p^2 divides R, with p prime to d*ad, then we can accumulate p into l,
     which will give an even smaller R' = R/p^2.
     Note: this might produce duplicate polynomials, since a given p*l
     might be found in different ways. For example with revision b5a1635 and
     polyselect -P 60000 -N 12939597433839929710052817774007139127064894178566832462175875720079522272519444917218095639720802504629187785806903263303 -degree 5 -t 1 -admin 780 -admax 840 -incr 60 -nq 2317
     the polynomial with Y1 = 35641965604484971 is found four times:
     * once with q = 92537 = 37 * 41 * 61
     * then with q = 182573 = 41 * 61 * 73
     * then with q = 110741 = 37 * 41 * 73
     * and finally with q = 164761 = 37 * 61 * 73
     As a workaround, we only allow p > qmax, the largest prime factor of q.
  */

  /* compute the largest prime factor of q */
  unsigned long qmax = 1;
  for (unsigned long j = 0; j < LEN_SPECIAL_Q - 1; j++)
    if ((q % SPECIAL_Q[j]) == 0)
      qmax = SPECIAL_Q[j];

  mpz_mul_ui (m, ad, d);
  mpz_pow_ui (m, m, d);
  mpz_divexact (m, m, ad);
  mpz_mul (m, m, N); /* m := Ntilde = d^d*ad^(d-1)*N */
  mpz_pow_ui (t, mtilde, d);
  mpz_sub (t, m, t);
  mpz_divexact (t, t, l);
  mpz_divexact (t, t, l);
  unsigned long p;
  prime_info pi;
  prime_info_init (pi);
  /* Note: we could find p^2 dividing t in a much more efficient way, for
     example by precomputing the product of all primes < 2*P, then doing
     a gcd with t, which gives say g, then computing gcd(t, t/g).
     But if P is small, it would gain little with respect to the naive loop
     below, and if P is large, we have only a few hits, thus the global
     overhead will be small too. */
  for (p = 2; p <= Primes[lenPrimes - 1]; p = getprime_mt (pi))
    {
      if (p <= qmax || d % p == 0 || mpz_divisible_ui_p (ad, p))
        continue;
      while (mpz_divisible_ui_p (t, p * p))
        {
          mpz_mul_ui (l, l, p);
          mpz_divexact_ui (t, t, p * p);
        }
    }
  prime_info_clear (pi);
  /* end of small improvement */

  /* we want mtilde = d*ad*m + a_{d-1}*l with -d*ad/2 <= a_{d-1} < d*ad/2.
     We have a_{d-1} = mtilde/l mod (d*ad). */
  mpz_mul_ui (m, ad, d);
  if (mpz_invert (adm1, l, m) == 0)
  {
    fprintf (stderr, "Error in 1/l mod (d*ad)\n");
    exit (1);
  }
  mpz_mul (adm1, adm1, mtilde);
  mpz_mod (adm1, adm1, m); /* m is d*ad here */

  /* we make -d*ad/2 <= adm1 < d*ad/2 */
  mpz_mul_2exp (t, adm1, 1);
  if (mpz_cmp (t, m) >= 0)
    mpz_sub (adm1, adm1, m);

  mpz_mul (m, adm1, l);
  mpz_sub (m, mtilde, m);
  check_divexact_ui (m, m, "m-a_{d-1}*l", d, "d");

  check_divexact (m, m, "(m-a_{d-1}*l)/d", ad, "ad");
  mpz_set (g[1], l);
  mpz_neg (g[0], m);
  mpz_set (f[d], ad);
  mpz_pow_ui (t, m, d);
  mpz_mul (t, t, ad);
  mpz_sub (t, N, t);
  mpz_set (f[d-1], adm1);
  check_divexact (t, t, "t", l, "l");
  mpz_pow_ui (mtilde, m, d-1);
  mpz_mul (mtilde, mtilde, adm1);
  mpz_sub (t, t, mtilde);
  for (unsigned long j = d - 2; j > 0; j--)
  {
    check_divexact (t, t, "t", l, "l");
    /* t = a_j*m^j + l*R thus a_j = t/m^j mod l */
    mpz_pow_ui (mtilde, m, j);
    /* fdiv rounds toward -infinity: adm1 = floor(t/mtilde) */
    mpz_fdiv_q (adm1, t, mtilde); /* t -> adm1 * mtilde + t */
    mpz_invert (k, mtilde, l); /* search adm1 + k such that
                                  t = (adm1 + k) * m^j mod l */
    mpz_mul (k, k, t);
    mpz_sub (k, k, adm1);
    mpz_mod (k, k, l);

    mpz_mul_2exp (k, k, 1);
    cmp = mpz_cmp (k, l);
    mpz_div_2exp (k, k, 1);
    if (cmp >= 0)
      mpz_sub (k, k, l);
    mpz_add (adm1, adm1, k);
    mpz_set (f[j], adm1);
    /* subtract adm1*m^j */
    mpz_submul (t, mtilde, adm1);
  }
  check_divexact (t, t, "t", l, "l");
  mpz_set (f[0], t);

  /* As noticed by Min Yang, Qingshu Meng, Zhangyi Wang, Lina Wang and
     Huanguo Zhang in "Polynomial Selection for the Number Field Sieve in an
     Elementary Geometric View" (https://eprint.iacr.org/2013/583),
     if the coefficient of degree d-2 is of the same sign as the leading
     coefficient, the size optimization will not work well, thus we simply
     discard those polynomials. */
  if (mpz_sgn (f[d]) * mpz_sgn (f[d-2]) > 0)
    {
#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
      discarded1 ++;
      goto end;
    }

  /* save unoptimized polynomial to fold */
  for (unsigned long j = d + 1; j -- != 0; )
    mpz_set (fold[j], f[j]);
  mpz_set (gold[1], g[1]);
  mpz_set (gold[0], g[0]);

  /* old lognorm */
  skew = L2_skewness (F, SKEWNESS_DEFAULT_PREC);
  logmu = L2_lognorm (F, skew);

#ifdef HAVE_OPENMP
#pragma omp critical
#endif
  {
    /* information on all polynomials */
    collisions ++;
    tot_found ++;
    aver_raw_lognorm += logmu;
    var_raw_lognorm += logmu * logmu;
    if (logmu < min_raw_lognorm)
      min_raw_lognorm = logmu;
    if (logmu > max_raw_lognorm)
      max_raw_lognorm = logmu;
  }

  /* if the polynomial has small norm, we optimize it */
  did_optimize = optimize_raw_poly (F, g);

  /* print optimized (maybe size- or size-root- optimized) polynomial */
  if (did_optimize && verbose >= 0)
    {
      unsigned long idx = get_idx (ad);
      output_polynomials (fold, d, gold, N, F->coeff, g, idx);
    }

 end:
  mpz_clear (l);
  mpz_clear (m);
  mpz_clear (t);
  mpz_clear (k);
  mpz_clear (adm1);
  mpz_clear (mtilde);
  mpz_clear (g[0]);
  mpz_clear (g[1]);
  mpz_clear (gold[0]);
  mpz_clear (gold[1]);
  mpz_poly_clear (F);
  for (unsigned long j = 0; j <= d; j++)
    mpz_clear (fold[j]);
  free (fold);
}


/* rq is a root of N = (m0 + rq)^d mod (q^2) */
/* this routine is called from polyselect_str.c */
void
gmp_match (uint32_t p1, uint32_t p2, int64_t i, mpz_t m0,
	   mpz_t ad, unsigned long d, mpz_t N, uint64_t q, mpz_t rq)
{
  mpz_t l, mtilde, m, adm1, t, k, *f, g[2], *fold, gold[2], qq, tmp;
  int cmp, did_optimize;
  double skew, logmu;
  mpz_poly F;

#ifdef DEBUG_POLYSELECT
  gmp_printf ("Found match: (%" PRIu32 ",%lld) (%" PRIu32 ",%lld) for "
	      "ad=%Zd, q=%llu, rq=%Zd\n",
              p1, (long long) i, p2, (long long) i, ad,
              (unsigned long long) q, rq);
  gmp_printf ("m0=%Zd\n", m0);
#endif
  mpz_init (tmp);
  mpz_init (l);
  mpz_init (m);
  mpz_init (t);
  mpz_init (k);
  mpz_init (qq);
  mpz_init (adm1);
  mpz_init (mtilde);
  mpz_init (g[0]);
  mpz_init (g[1]);
  mpz_init (gold[0]);
  mpz_init (gold[1]);
  mpz_poly_init (F, d);
  F->deg = d;
  f = F->coeff;
  fold = (mpz_t*) malloc ((d + 1) * sizeof (mpz_t));
  if (fold == NULL)
  {
    fprintf (stderr, "Error, cannot allocate memory in match\n");
    exit (1);
  }
  for (unsigned long j = 0; j <= d; j++)
    mpz_init (fold[j]);
  /* we have l = p1*p2*q */
  mpz_set_ui (l, p1);
  mpz_mul_ui (l, l, p2);
  mpz_set_uint64 (tmp, q);
  mpz_mul (l, l, tmp);
  /* mtilde = m0 + rq + i*q^2 */
  mpz_set (qq, tmp); // qq = q
  mpz_mul (qq, qq, tmp); // qq = q^2
  if (i >= 0)
    mpz_set_uint64 (tmp, (uint64_t) i);
  else {
    mpz_set_uint64 (tmp, (uint64_t) (-i));
    mpz_neg (tmp, tmp);
  }
  mpz_set (mtilde, tmp);
  mpz_mul (mtilde, mtilde, qq);
  mpz_add (mtilde, mtilde, rq);
  mpz_add (mtilde, mtilde, m0);
  /* we want mtilde = d*ad*m + a_{d-1}*l with 0 <= a_{d-1} < d*ad.
     We have a_{d-1} = mtilde/l mod (d*ad). */
  mpz_mul_ui (m, ad, d);
  if (mpz_invert (adm1, l, m) == 0)
  {
    fprintf (stderr, "Error in 1/l mod (d*ad)\n");
    exit (1);
  }
  mpz_mul (adm1, adm1, mtilde);
  mpz_mod (adm1, adm1, m); /* m is d*ad here */
  /* we make -d*ad/2 <= adm1 < d*ad/2 */
  mpz_mul_2exp (t, adm1, 1);
  if (mpz_cmp (t, m) >= 0)
    mpz_sub (adm1, adm1, m);
  mpz_mul (m, adm1, l);
  mpz_sub (m, mtilde, m);
  check_divexact_ui (m, m, "m-a_{d-1}*l", d, "d");
  check_divexact (m, m, "(m-a_{d-1}*l)/d", ad, "ad");
  mpz_set (g[1], l);
  mpz_neg (g[0], m);
  mpz_set (f[d], ad);
  mpz_pow_ui (t, m, d);
  mpz_mul (t, t, ad);
  mpz_sub (t, N, t);
  mpz_set (f[d-1], adm1);
  check_divexact (t, t, "t", l, "l");
  mpz_pow_ui (mtilde, m, d-1);
  mpz_mul (mtilde, mtilde, adm1);
  mpz_sub (t, t, mtilde);
  for (unsigned long j = d - 2; j > 0; j--)
  {
    check_divexact (t, t, "t", l, "l");
    /* t = a_j*m^j + l*R thus a_j = t/m^j mod l */
    mpz_pow_ui (mtilde, m, j);
    mpz_fdiv_q (adm1, t, mtilde); /* t -> adm1 * mtilde + t */
    mpz_invert (k, mtilde, l); /* search adm1 + k such that
                                  t = (adm1 + k) * m^j mod l */
    mpz_mul (k, k, t);
    mpz_sub (k, k, adm1);
    mpz_mod (k, k, l);
    mpz_mul_2exp (k, k, 1);
    cmp = mpz_cmp (k, l);
    mpz_div_2exp (k, k, 1);
    if (cmp >= 0)
      mpz_sub (k, k, l);
    mpz_add (adm1, adm1, k);
    mpz_set (f[j], adm1);
    /* subtract adm1*m^j */
    mpz_submul (t, mtilde, adm1);
  }

  check_divexact (t, t, "t", l, "l");
  mpz_set (f[0], t);

  /* save unoptimized polynomial to fold */
  for (i = d + 1; i -- != 0; )
    mpz_set (fold[i], f[i]);
  mpz_set (gold[1], g[1]);
  mpz_set (gold[0], g[0]);

  /* old lognorm */
  skew = L2_skewness (F, SKEWNESS_DEFAULT_PREC);
  logmu = L2_lognorm (F, skew);

#ifdef HAVE_OPENMP
#pragma	omp critical
#endif
  {
    /* information on all polynomials */
    collisions ++;
    tot_found ++;
    aver_raw_lognorm += logmu;
    var_raw_lognorm += logmu * logmu;
    if (logmu < min_raw_lognorm)
      min_raw_lognorm = logmu;
    if (logmu > max_raw_lognorm)
      max_raw_lognorm = logmu;
  }

  /* if the polynomial has small norm, we optimize it */
  did_optimize = optimize_raw_poly (F, g);

  /* print optimized (maybe size- or size-root- optimized) polynomial */
  if (did_optimize && verbose >= 0)
    {
      unsigned long idx = get_idx (ad);
      output_polynomials (fold, d, gold, N, F->coeff, g, idx);
    }

  mpz_clear (tmp);
  mpz_clear (l);
  mpz_clear (m);
  mpz_clear (t);
  mpz_clear (k);
  mpz_clear (qq);
  mpz_clear (adm1);
  mpz_clear (mtilde);
  mpz_clear (g[0]);
  mpz_clear (g[1]);
  mpz_clear (gold[0]);
  mpz_clear (gold[1]);
  mpz_poly_clear (F);
  for (unsigned long j = 0; j <= d; j++)
    mpz_clear (fold[j]);
  free (fold);
}


/* find collisions between "P" primes, return number of loops */
static inline unsigned long
collision_on_p (header_t header, proots_t R, shash_t H)
{
  unsigned long j, nprimes, p, nrp, c = 0, tot_roots = 0;
  uint64_t *rp;
  int64_t ppl = 0, u, umax;
  mpz_t zero;
  int found = 0;
  int st = milliseconds ();

  /* init zero */
  mpz_init_set_ui (zero, 0);

  rp = (uint64_t*) malloc (header->d * sizeof (uint64_t));
  if (rp == NULL) {
    fprintf (stderr, "Error, cannot allocate memory in collision_on_p\n");
    exit (1);
  }

  shash_reset (H);
  umax = (int64_t) Primes[lenPrimes - 1] * (int64_t) Primes[lenPrimes - 1];
  for (nprimes = 0; nprimes < lenPrimes; nprimes ++)
    {
      p = Primes[nprimes];
      ppl = (int64_t) p * (int64_t) p;

      /* add fake roots to keep indices */
      if (header_skip (header, p))
        {
          R->nr[nprimes] = 0; // nr = 0.
          R->roots[nprimes] = NULL;
          continue;
        }

      nrp = roots_mod_uint64 (rp, mpz_fdiv_ui (header->Ntilde, p), header->d,
                              p);
      tot_roots += nrp;
      nrp = roots_lift (rp, header->Ntilde, header->d, header->m0, p, nrp);
      proots_add (R, nrp, rp, nprimes);
      for (j = 0; j < nrp; j++, c++)
            {
              for (u = (int64_t) rp[j]; u < umax; u += ppl)
                shash_add (H, u);
              for (u = ppl - (int64_t) rp[j]; u < umax; u += ppl)
                shash_add (H, -u);
            }
        }
  found = shash_find_collision (H);
  free (rp);
  st = milliseconds () - st;

  if (verbose > 2)
    fprintf (stderr, "# computing %lu p-roots took %dms\n", tot_roots, st);

  if (found) /* do the real work */
    {
      hash_t H;

      hash_init (H, INIT_FACTOR * lenPrimes);
      for (nprimes = 0; nprimes < lenPrimes; nprimes ++)
        {
          nrp = R->nr[nprimes];
          if (nrp == 0)
            continue;
          p = Primes[nprimes];
          ppl = (int64_t) p * (int64_t) p;
          rp = R->roots[nprimes];

          for (j = 0; j < nrp; j++)
            {
              for (u = (int64_t) rp[j]; u < umax; u += ppl)
                hash_add (H, p, u, header->m0, header->ad, header->d,
                          header->N, 1, zero);
              for (u = ppl - (int64_t) rp[j]; u < umax; u += ppl)
                hash_add (H, p, -u, header->m0, header->ad,
                          header->d, header->N, 1, zero);
            }
        }
#ifdef DEBUG_POLYSELECT
      fprintf (stderr, "# collision_on_p took %lums\n", milliseconds () - st);
      gmp_fprintf (stderr, "# p hash_size: %u for ad = %Zd\n",
                   H->size, header->ad);
#endif

#ifdef DEBUG_HASH_TABLE
      fprintf (stderr, "# p hash_size: %u, hash_alloc: %u\n", H->size, H->alloc);
      fprintf (stderr, "# hash table coll: %lu, all_coll: %lu\n", H->coll, H->coll_all);
#endif
      hash_clear (H);
    }

  mpz_clear (zero);

#ifdef HAVE_OPENMP
#pragma	omp atomic update
#endif
  potential_collisions ++;
  return c;
}


/* collision on each special-q, call collision_on_batch_p() */
static inline void
collision_on_each_sq ( header_t header,
                       proots_t R,
                       unsigned long q,
                       mpz_t rqqz,
                       unsigned long *inv_qq,
                       shash_t H )
{
  uint64_t **cur1, **cur2, *ccur1, *ccur2;
  long *pc, *epc;
  uint64_t pp;
  int64_t ppl, neg_umax, umax, v1, v2, nv;
  unsigned long p, nprimes, c;
  uint8_t vpnr, *pnr, nr, j;
  uint32_t *pprimes, i;
  int found;

#ifdef DEBUG_POLYSELECT
  int st = milliseconds();
#endif
#if SHASH_NBUCKETS == 256
#define CURRENT(V) (H->current + (uint8_t) (V))
#else
#define CURRENT(V) (H->current + ((V) & (SHASH_NBUCKETS - 1)))
#endif

  shash_reset (H);

  pc = (long *) inv_qq;
  nv = *pc;
  pprimes = Primes - 1;
  pnr = R->nr;
  R->nr[R->size] = 0xff; /* I use guard to end */
  umax = Primes[lenPrimes - 1];
  umax *= umax;
  neg_umax = -umax;

  /* This define inserts 2 values v1 and v2 with a interlace.
     The goal is to have a little time to read ccurX from L0
     cache before to use it. The best seems a
     three read interlacing in fact, two seems too short. */
#define INSERT_2I(I1,I2)                                                \
  do {                                                                  \
    cur1 = CURRENT(I1); ccur1 = *cur1;					\
    cur2 = CURRENT(I2); ccur2 = *cur2;					\
    *ccur1++ = I1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;	\
    *ccur2++ = I2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;	\
  } while (0)
  /* This version is slow because ccur1 is used immediatly after
     it has been read from L0 cache -> 3 ticks of latency on P4 Nehalem. */
#define INSERT_I(I)						\
  do {								\
    cur1 = CURRENT(I); ccur1 = *cur1; *ccur1++ = I;		\
    __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;		\
  } while (0)

  int64_t b;
  b = (int64_t) ((double) umax * 0.3333333333333333);
  do {
    do {
      vpnr = *pnr++;
      pprimes++;
    } while (!vpnr);
    if (UNLIKELY(vpnr == 0xff)) goto bend;
    ppl = *pprimes;
    __builtin_prefetch(((void *) pnr) + 0x040, 0, 3);
    __builtin_prefetch(((void *) pprimes) + 0x80, 0, 3);
    __builtin_prefetch(((void *) pc) + 0x100, 0, 3);
    ppl *= ppl;
    epc = pc + vpnr;
    if (UNLIKELY(ppl > b)) { b = umax >> 1; goto iter2; }
    do {
      v1 = nv;                    cur1 = CURRENT(v1); ccur1 = *cur1;
      v2 = v1 - ppl;              cur2 = CURRENT(v2); ccur2 = *cur2;
      nv = *++pc;
      *ccur1++ = v1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;
      *ccur2++ = v2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;
      v1 += ppl;                  cur1 = CURRENT(v1); ccur1 = *cur1;
      v2 -= ppl;                  cur2 = CURRENT(v2); ccur2 = *cur2;
      *ccur1++ = v1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;
      *ccur2++ = v2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;
      v1 += ppl;                  cur1 = CURRENT(v1); ccur1 = *cur1;
      v2 -= ppl;                  cur2 = CURRENT(v2); ccur2 = *cur2;
      *ccur1++ = v1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;
      *ccur2++ = v2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;
      v1 += ppl; v2 -= ppl;
      if (LIKELY (v1 > umax)) {
	if (UNLIKELY (v2 >= neg_umax)) INSERT_I(v2);
      } else if (UNLIKELY (v2 >= neg_umax)) INSERT_2I(v1, v2);
      else INSERT_I(v1);
    } while (pc != epc);
  } while (1);

  do {
    do {
      vpnr = *pnr++;
      pprimes++;
    } while (!vpnr);
    if (UNLIKELY(vpnr == 0xff)) goto bend;
    ppl = *pprimes;
    __builtin_prefetch(((void *) pnr) + 0x040, 0, 3);
    __builtin_prefetch(((void *) pprimes) + 0x100, 0, 3);
    __builtin_prefetch(((void *) pc) + 0x280, 0, 3);
    ppl *= ppl;
    epc = pc + vpnr;
  iter2:
    if (UNLIKELY(ppl > b)) goto iter1;
    do {
      v1 = nv;                    cur1 = CURRENT(v1); ccur1 = *cur1;
      v2 = v1 - ppl;              cur2 = CURRENT(v2); ccur2 = *cur2;
      nv = *++pc;
      *ccur1++ = v1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;
      *ccur2++ = v2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;
      v1 += ppl;                  cur1 = CURRENT(v1); ccur1 = *cur1;
      v2 -= ppl;                  cur2 = CURRENT(v2); ccur2 = *cur2;
      *ccur1++ = v1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;
      *ccur2++ = v2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;
      v1 += ppl; v2 -= ppl;
      if (LIKELY (v1 > umax)) {
	if (UNLIKELY (v2 >= neg_umax)) INSERT_I(v2);
      } else if (UNLIKELY (v2 >= neg_umax)) INSERT_2I(v1, v2);
      else INSERT_I(v1);
    } while (pc != epc);
  } while (1);

  do {
    do {
      vpnr = *pnr++;
      pprimes++;
    } while (!vpnr);
    if (UNLIKELY(vpnr == 0xff)) goto bend;
    ppl = *pprimes;
    __builtin_prefetch(((void *) pnr) + 0x040, 0, 3);
    __builtin_prefetch(((void *) pprimes) + 0x100, 0, 3);
    __builtin_prefetch(((void *) pc) + 0x280, 0, 3);
    ppl *= ppl;
    epc = pc + vpnr;
  iter1:
    do {
      v1 = nv;                    cur1 = CURRENT(v1); ccur1 = *cur1;
      v2 = v1 - ppl;              cur2 = CURRENT(v2); ccur2 = *cur2;
      nv = *++pc;
      *ccur1++ = v1; __builtin_prefetch(ccur1, 1, 3); *cur1 = ccur1;
      *ccur2++ = v2; __builtin_prefetch(ccur2, 1, 3); *cur2 = ccur2;
      v1 += ppl; v2 -= ppl;
      if (LIKELY (v1 > umax)) {
	if (UNLIKELY (v2 >= neg_umax)) INSERT_I(v2);
      } else if (UNLIKELY (v2 >= neg_umax)) INSERT_2I(v1, v2);
      else INSERT_I(v1);
    } while (pc != epc);
  } while (1);

 bend:
#undef INSERT_2I
#undef INSERT_I

  for (i = 0; i < SHASH_NBUCKETS; i++) ASSERT (H->current[i] <= H->base[i+1]);

  found = shash_find_collision (H);

  if (found) /* do the real work */
    {
      hash_t H;

      hash_init (H, INIT_FACTOR * lenPrimes);

      umax = (int64_t) Primes[lenPrimes - 1] * (int64_t) Primes[lenPrimes - 1];
      for (nprimes = c = 0; nprimes < lenPrimes; nprimes ++)
        {
          p = Primes[nprimes];
          if (header_skip (header, p))
            continue;
          pp = p * p;
          ppl = (long) pp;
          nr = R->nr[nprimes];
          for (j = 0; j < nr; j++, c++)
            {
              v1 = (long) inv_qq[c];
              for (v2 = v1; v2 < umax; v2 += ppl)
                hash_add (H, p, v2, header->m0, header->ad, header->d,
                          header->N, q, rqqz);
              for (v2 = ppl - v1; v2 < umax; v2 += ppl)
                hash_add (H, p, -v2, header->m0, header->ad, header->d,
                          header->N, q, rqqz);
            }
        }
      hash_clear (H);
    }

#ifdef DEBUG_POLYSELECT
  fprintf (stderr, "# inner collision_on_each_sq took %lums\n", milliseconds () - st);
  fprintf (stderr, "# - q hash_alloc (q=%lu): %u\n", q, H->alloc);
#endif

#ifdef DEBUG_HASH_TABLE
  fprintf (stderr, "# p hash_size: %u, hash_alloc: %u\n", H->size, H->alloc);
  fprintf (stderr, "# hash table coll: %lu, all_coll: %lu\n", H->coll, H->coll_all);
#endif

#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
  potential_collisions ++;
}


/* Given p, rp, q, invqq[], for each rq of q, compute (rp - rq) / q^2 */
static inline void
collision_on_each_sq_r ( header_t header,
                         proots_t R,
                         unsigned long q,
                         mpz_t *rqqz,
                         unsigned long *inv_qq,
                         unsigned long number_pr,
                         int count,
                         shash_t H )
{
  if (count == 0)
    return;

  uint8_t i, nr, *pnr;
  unsigned long nprimes, p, c = 0, rp, rqi;
  int k;
  uint64_t pp;
  unsigned long **tinv_qq = malloc (count * sizeof (unsigned long*));

  if (!tinv_qq)
  {
    fprintf (stderr, "Error, cannot allocate memory in %s\n", __FUNCTION__);
    exit (1);
  }
  for (k = 0; k < count; k++) {
    /* number_pr + 1 for guard for pre-load in collision_on_each_sq (nv) */
    tinv_qq[k] = malloc ((number_pr + 1) * sizeof (unsigned long));
    tinv_qq[k][number_pr] = 0;
  }

  int st = milliseconds();
  pnr = R->nr;

  /* for each rp, compute (rp-rq)*1/q^2 (mod p^2) */
  for (nprimes = 0; nprimes < lenPrimes; nprimes ++)
  {
    if (!pnr[nprimes]) continue;
    nr = pnr[nprimes];
    p = Primes[nprimes];
    pp = p*p;

    modulusredcul_t modpp;
    residueredcul_t res_rqi, res_rp, res_tmp;
    modredcul_initmod_ul_raw (modpp, pp);
    modredcul_init (res_rqi, modpp);
    modredcul_init (res_rp, modpp);
    modredcul_init (res_tmp, modpp);

    for (k = 0; k < count; k ++)
    {
      rqi = mpz_fdiv_ui (rqqz[k], pp);
      modredcul_intset_ul (res_rqi, rqi);
      modredcul_intset_ul (res_tmp, inv_qq[nprimes]);
      for (i = 0; i < nr; i ++, c++)
      {
        rp = R->roots[nprimes][i];
        modredcul_intset_ul (res_rp, rp);
        /* rp - rq */
        modredcul_sub (res_rp, res_rp, res_rqi, modpp);
        /* res_rp = (rp - rq) / q[i]^2 */
        modredcul_mul (res_rp, res_rp, res_tmp, modpp);
        tinv_qq[k][c] = modredcul_intget_ul (res_rp);
      }
      c -= nr;
    }
    c += nr;

    modredcul_clear (res_rp, modpp);
    modredcul_clear (res_rqi, modpp);
    modredcul_clear (res_tmp, modpp);
    modredcul_clearmod (modpp);
  }

  if (verbose > 2) {
    fprintf (stderr, "#  substage: batch %d many (rp-rq)*1/q^2 took %lums\n",
             count, milliseconds () - st);
    st = milliseconds();
  }

  /* core function to find collisions */
  shash_reset (H);
  for (k = 0; k < count; k ++)
    collision_on_each_sq (header, R, q, rqqz[k], tinv_qq[k], H);

  if (verbose > 2)
    fprintf (stderr, "#  substage: collision-detection %d many rq took %lums\n",
             count, milliseconds () - st);

  for (k = 0; k < count; k++)
    free (tinv_qq[k]);
  free (tinv_qq);
}


/* Next combination */
static inline unsigned int
aux_nextcomb ( unsigned int *ind,
               unsigned int len_q,
               unsigned int *len_nr )
{
  unsigned int i;

  /* bottom change first */
  for (i = len_q - 1; ; i--) {
    if (ind[i] < (len_nr[i] - 1)) {
      ind[i]++;
      return 1;
    }
    else {
      if (i == 0)
        break;
      ind[i] = 0;
    }
  }
  return 0;
}


/* Compute crt-ed rq (qqz,rqqz) = (q_1 * ... * q_k,
                                   CRT([r_1, ..., r_k], [q_1, ..., q_k])) */
static inline void
aux_return_rq ( qroots_t SQ_R,
                unsigned long *idx_q,
                unsigned int *idx_nr,
                unsigned long k,
                mpz_t qqz,
                mpz_t rqqz)
{
  unsigned long i, q[k], rq[k];

  /* q and roots */
  for (i = 0; i < k; i ++) {
    q[i] = SQ_R->q[idx_q[i]];
    rq[i] = SQ_R->roots[idx_q[i]][idx_nr[i]];
  }

  /* crt roots */
  crt_sq (qqz, rqqz, q, rq, k);

  return;
}


/* Consider each rq which is the product of k pairs (q,r).
   In this routine the q[i] are fixed, only the roots mod q[i] change. */
static inline void
collision_on_batch_sq_r ( header_t header,
                          proots_t R,
                          qroots_t SQ_R,
                          unsigned long q,
                          unsigned long *idx_q,
                          unsigned long *inv_qq,
                          unsigned long number_pr,
                          unsigned long *curr_nq,
                          unsigned long k,
                          shash_t H)
{
  int count;
  unsigned int ind_qr[k]; /* indices of roots for each small q */
  unsigned int len_qnr[k]; /* for each small q, number of roots */
  unsigned long i;
  mpz_t qqz, rqqz[BATCH_SIZE];

  mpz_init (qqz);
  for (i = 0; i < BATCH_SIZE; i ++)
    mpz_init (rqqz[i]);

  /* initialization indices */
  for (i = 0; i < k; i ++) {
    ind_qr[i] = 0;
    len_qnr[i] = SQ_R->nr[idx_q[i]];
  }

#if 0
  fprintf (stderr, "q: %lu, ", q);
  for (i = 0; i < k; i ++)
    fprintf (stderr, "%u ", SQ_R->q[idx_q[i]]);
  fprintf (stderr, ", ");
  for (i = 0; i < k; i ++)
    fprintf (stderr, "%u ", SQ_R->nr[idx_q[i]]);
  fprintf (stderr, "\n");
#endif

  /* we proceed with BATCH_SIZE many rq for each time */
  int re = 1, num_rq;
  while (re) {
    /* compute BATCH_SIZE such many rqqz[] */
    num_rq = 0;
    for (count = 0; count < BATCH_SIZE; count ++)
    {
        aux_return_rq (SQ_R, idx_q, ind_qr, k, qqz, rqqz[count]);
        re = aux_nextcomb (ind_qr, k, len_qnr);
        (*curr_nq)++;
        num_rq ++;
        if ((*curr_nq) >= nq)
          re = 0;
        if (!re)
          break;
    }

    /* core function for a fixed qq and several rqqz[] */
    collision_on_each_sq_r (header, R, q, rqqz, inv_qq, number_pr, num_rq, H);
  }

  mpz_clear (qqz);
  for (i = 0; i < BATCH_SIZE; i ++)
    mpz_clear (rqqz[i]);
}


/* SQ inversion, write 1/q^2 (mod p_i^2) to invqq[i].
   In this routine the q[i] are fixed, corresponding to indices idx_q[0], ...,
   idx_q[k-1] */
static inline void
collision_on_batch_sq (header_t header,
                       proots_t R,
                       qroots_t SQ_R,
                       unsigned long q,
                       unsigned long *idx_q,
                       unsigned long number_pr,
                       unsigned long k,
                       unsigned long *curr_nq,
                       shash_t H)
{
  unsigned nr;
  uint64_t pp;
  unsigned long nprimes, p;
  unsigned long *invqq = malloc (lenPrimes * sizeof (unsigned long));
  if (!invqq) {
    fprintf (stderr, "Error, cannot allocate memory in %s\n", __FUNCTION__);
    exit (1);
  }

  int st = milliseconds();

  /* Step 1: inversion */
  for (nprimes = 0; nprimes < lenPrimes; nprimes ++) {

    p = Primes[nprimes];
    if (header_skip (header, p))
      continue;
    nr = R->nr[nprimes];
    if (nr == 0)
      continue;
    pp = p * p;

    modulusredcul_t modpp;
    residueredcul_t qq, tmp;
    modredcul_initmod_ul (modpp, pp);
    modredcul_init (qq, modpp);
    modredcul_init (tmp, modpp);

    /* q^2/B (mod pp). Warning: for large nq, we might have q > p^2, therefore
       we must first reduce q mod p^2 before calling modredcul_intset_ul. */
    modredcul_intset_ul (tmp, q % pp);
    modredcul_sqr (qq, tmp, modpp);
    /* B/q^2 (mod pp) */
    modredcul_intinv (tmp, qq, modpp);
    invqq[nprimes] = modredcul_intget_ul (tmp);

    modredcul_clear (tmp, modpp);
    modredcul_clear (qq, modpp);
    modredcul_clearmod (modpp);
  }

  if (verbose > 2)
    fprintf (stderr, "# stage (1/q^2 inversion) for %lu primes took %lums\n",
             lenPrimes, milliseconds () - st);

  /* Step 2: find collisions on q. */
  int st2 = milliseconds();

  collision_on_batch_sq_r (header, R, SQ_R, q, idx_q, invqq, number_pr,
                           curr_nq, k, H);
  if (verbose > 2)
    fprintf (stderr, "#  stage (special-q) for %lu special-q's took %lums\n",
             *curr_nq, milliseconds() - st2);

  free (invqq);
}

/* find suitable values of lq and k, where the special-q part in the degree-1
   coefficient of the linear polynomial is made from k small primes among lq */
static unsigned long
find_suitable_lq (header_t header, qroots_t SQ_R, unsigned long *k)
{
  unsigned long prod = 1;
  unsigned int i;
  double sq = 1.0;
  unsigned long lq;

  for (i = 0, *k = 0; prod < nq && i < SQ_R->size; i++) {
    if (!check_parameters (header->m0, sq * (double) SQ_R->q[i]))
      break;
    prod *= header->d; /* We multiply by d instead of SQ_R->nr[i] to limit
                          the number of primes and thus the Y1 value. */
    sq *= (double) SQ_R->q[i];
    *k += 1;
  }

  /* We force k <= 4 on a 32-bit machine, and k <= 8 on a 64-bit machine,
     to ensure q fits on an "unsigned long". */
  if (*k > (sizeof (unsigned long) * CHAR_BIT) / 8)
    *k = (sizeof (unsigned long) * CHAR_BIT) / 8;
  if (*k < 1)
    *k = 1;

  /* If all factors in sq have d roots, then a single special-q is enough.
     Otherwise, we consider special-q's from combinations of k primes among lq,
     so that the total number of combinations is at least nq. */
  for (lq = *k; number_comb (SQ_R, *k, lq) < nq && lq < SQ_R->size; lq++);

  return lq;
}

/* collision on special-q, call collision_on_batch_sq */
static inline void
collision_on_sq (header_t header, proots_t R, unsigned long c, shash_t H)
{
  unsigned long k, lq;
  qroots_t SQ_R;

  /* init special-q roots */
  qroots_init (SQ_R);
  comp_sq_roots (header, SQ_R);
  //qroots_print (SQ_R);

  /* find a suitable lq */
  lq = find_suitable_lq (header, SQ_R, &k);

  unsigned long q, idx_q[lq], curr_nq = 0;

  first_comb (k, idx_q);
  while (curr_nq < nq)
    {
      q = return_q_norq (SQ_R, idx_q, k);

      /* collision batch */
      collision_on_batch_sq (header, R, SQ_R, q, idx_q, c, k, &curr_nq, H);
      next_comb (lq, k, idx_q);
    }

  /* clean */
  qroots_clear (SQ_R);
}


// separator between modredc_ul and gmp


/* find collisions between "P" primes, return number of loops */
static inline unsigned long
gmp_collision_on_p ( header_t header, proots_t R )
{
  unsigned long j, nprimes, p, nrp, c = 0;
  uint64_t *rp;
  int64_t ppl = 0, u, umax;
  mpz_t zero;

  /* init zero */
  mpz_init_set_ui (zero, 0);

  rp = (uint64_t*) malloc (header->d * sizeof (uint64_t));
  if (rp == NULL) {
    fprintf (stderr, "Error, cannot allocate memory in collision_on_p\n");
    exit (1);
  }

  hash_t H;

  hash_init (H, INIT_FACTOR * lenPrimes);

#ifdef DEBUG_POLYSELECT
  int st = milliseconds();
#endif

  umax = (int64_t) Primes[lenPrimes - 1] * (int64_t) Primes[lenPrimes - 1];
  for (nprimes = 0; nprimes < lenPrimes; nprimes ++) {
    p = Primes[nprimes];
    ppl = (int64_t) p * (int64_t) p;

    /* add fake roots to keep indices */
    if (header_skip (header, p)) {
      R->nr[nprimes] = 0; // nr = 0.
      R->roots[nprimes] = NULL;
      continue;
    }

    /* we want p^2 | N - (m0 + i)^d, thus
       (m0 + i)^d = N (mod p^2) or m0 + i = N^(1/d) mod p^2 */
    nrp = roots_mod_uint64 (rp, mpz_fdiv_ui (header->Ntilde, p), header->d, p);
    roots_lift (rp, header->Ntilde, header->d, header->m0, p, nrp);
    proots_add (R, nrp, rp, nprimes);
    for (j = 0; j < nrp; j++, c++) {
      for (u = (int64_t) rp[j]; u < umax; u += ppl)
        gmp_hash_add (H, p, u, header->m0, header->ad,
                      header->d, header->N, 1, zero);
      for (u = ppl - (int64_t) rp[j]; u < umax; u += ppl)
        gmp_hash_add (H, p, -u, header->m0, header->ad,
                      header->d, header->N, 1, zero);
    }
  }

#ifdef DEBUG_POLYSELECT
  fprintf (stderr, "# collision_on_p took %lums\n", milliseconds () - st);
  gmp_fprintf (stderr, "# p hash_size: %u for ad = %Zd\n", H->size,
               header->ad);
#endif

  hash_clear (H);

  free (rp);
  mpz_clear (zero);

#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
  potential_collisions ++;
  return c;
}


/* collision on each special-q, call collision_on_batch_p() */
static inline void
gmp_collision_on_each_sq ( header_t header,
			   proots_t R,
			   uint64_t q,
			   mpz_t rqqz,
			   uint64_t *inv_qq )
{
  unsigned int nr, j;
  unsigned long nprimes, p, c = 0;
  uint64_t pp;
  int64_t ppl, u, v, umax;

#ifdef DEBUG_POLYSELECT
  int st = milliseconds();
#endif

  hash_t H;

  hash_init (H, INIT_FACTOR * lenPrimes);

  umax = (int64_t) Primes[lenPrimes - 1] * (int64_t) Primes[lenPrimes - 1];
  for (nprimes = 0; nprimes < lenPrimes; nprimes ++) {

    p = Primes[nprimes];
    if (header_skip (header, p))
      continue;

    /* set p, p^2, ppl */
    pp = (uint64_t) p;
    pp *= (uint64_t) p;
    ppl = (int64_t) pp;
    nr = R->nr[nprimes];

    for (j = 0; j < nr; j++, c++)
    {
      u = (int64_t) inv_qq[c];

      for (v = u; v < umax; v += ppl)
        gmp_hash_add (H, p, v, header->m0, header->ad, header->d,
                      header->N, q, rqqz);
      for (v = ppl - u; v < umax; v += ppl)
        gmp_hash_add (H, p, -v, header->m0, header->ad, header->d,
                      header->N, q, rqqz);

    }  // next rp
  } // next p

#ifdef DEBUG_POLYSELECT
  fprintf (stderr, "# inner collision_on_each_sq took %lums\n",
	   milliseconds () - st);
  fprintf (stderr, "# - q hash_size (q=%lu): %u\n", q, H->size);
#endif

  hash_clear (H);

#ifdef HAVE_OPENMP
#pragma omp atomic update
#endif
  potential_collisions ++;
}


/* Batch SQ mode */
static inline void
gmp_collision_on_batch_sq ( header_t header,
			    proots_t R,
			    uint64_t *q,
			    mpz_t *qqz,
			    mpz_t *rqqz,
			    unsigned long size,
          unsigned long number_pr )
{
  if (size == 0)
    return;

  unsigned int i, j, nr;
  unsigned long nprimes, p, c = 0;
  uint64_t pp, **invqq, rp;
  mpz_t qprod[size], modpp, tmp, tmp1, tmp2, rpmp;

  mpz_init (tmp);
  mpz_init (tmp1);
  mpz_init (tmp2);
  mpz_init (rpmp);
  mpz_init (modpp);
  for (i = 0; i < size; i++)
    mpz_init (qprod[i]);

  invqq = (uint64_t **) malloc (size * sizeof (uint64_t *));
  if (invqq) {
    for (i = 0; i < size; i++)
      invqq[i] = malloc (number_pr * sizeof (uint64_t));
  }
  else {
    fprintf (stderr, "Error, cannot allocate memory in %s\n", __FUNCTION__);
    exit (1);
  }

  mpz_set (qprod[0], qqz[0]);
  for (i = 1; i < size; i ++)
    mpz_mul(qprod[i], qqz[i], qprod[i-1]);

  /* Step 1: batch inversion */
  for (nprimes = 0; nprimes < lenPrimes; nprimes ++) {

    p = Primes[nprimes];
    pp = (uint64_t) p;
    pp *= (uint64_t) p;
    if (header_skip (header, p))
      continue;
    if (R->nr[nprimes] == 0)
      continue;
    nr = R->nr[nprimes];

    /* inversion */
    mpz_set_uint64 (modpp, pp);
    mpz_invert (tmp1 ,qprod[size-1], modpp);

    /* for each (q, r) \in a batch */
    for (i = size-1; i > 0; i --) {
      mpz_mul(tmp, qprod[i-1], tmp1);
      /* for each rp, compute (rp-rq)*1/q^2 (mod p^2) */
      for (j = 0; j < nr; j ++, c++) {
        rp = R->roots[nprimes][j];
	mpz_set_uint64 (rpmp, rp);
	mpz_sub (rpmp, rpmp, rqqz[i]);
	mpz_mul (rpmp, rpmp, tmp);
	mpz_mod (rpmp, rpmp, modpp);
        invqq[i][c] = mpz_get_uint64 (rpmp);
      }
      mpz_mul (tmp, qqz[i], tmp1);
      mpz_mod (tmp1, tmp, modpp);
      c -= nr;
    }
    /* last q in the batch is in tmp_modul */
    for (j = 0; j < nr; j ++, c ++) {
      rp = R->roots[nprimes][j];
      mpz_set_uint64 (rpmp, rp);
      mpz_sub (rpmp, rpmp, rqqz[0]);
      mpz_fdiv_r (rpmp, rpmp, modpp);
      mpz_mul (tmp2, rpmp, tmp1);
      mpz_mod (tmp2, tmp2, modpp);
      invqq[0][c] = mpz_get_uint64 (tmp2);
    }
  } // next prime p

  /* Step 2: find collisions on q. */
  for (i = 0; i < size; i ++) {
    //int st2 = milliseconds();
    gmp_collision_on_each_sq (header, R, q[i], rqqz[i], invqq[i]);
    //printf ("# outer collision_on_each_sq took %dms\n", milliseconds () - st2);
  }

  for (i = 0; i < size; i++)
    free (invqq[i]);
  free (invqq);
  mpz_clear (tmp);
  mpz_clear (tmp1);
  mpz_clear (tmp2);
  mpz_clear (rpmp);
  mpz_clear (modpp);
  for (i = 0; i < size; i++)
    mpz_clear (qprod[i]);
}


/* collision on special-q, call gmp_collision_on_batch_sq */
static inline void
gmp_collision_on_sq ( header_t header,
		      proots_t R,
		      unsigned long c )
{
  // init special-q roots
  unsigned long K, lq;
  qroots_t SQ_R;
  qroots_init (SQ_R);
  comp_sq_roots (header, SQ_R);
  // qroots_print (SQ_R);

  /* find a suitable lq */
  lq = find_suitable_lq (header, SQ_R, &K);

  unsigned long N = lq, tot, i, l, idx_q[K];
  uint64_t q[BATCH_SIZE];
  mpz_t *qqz, *rqqz;

  qqz = (mpz_t*) malloc (BATCH_SIZE * sizeof (mpz_t));
  rqqz = (mpz_t*) malloc (BATCH_SIZE * sizeof (mpz_t));
  if (!qqz || !rqqz) {
    fprintf (stderr, "Error, cannot allocate memory "
	     "in gmp_collision_on_sq \n");
    exit (1);
  }
  for (l = 0; l < BATCH_SIZE; l++) {
    mpz_init (qqz[l]);
    mpz_init (rqqz[l]);
  }

  // less than lq special primes having roots for this ad
  if (N == 0 || N < K) {
    gmp_fprintf (stderr, "# Info: binomial(%lu, %lu) error in "
                 "collision_on_sq(). ad=%Zd.\n", N, K, header->ad);
    return;
  }

  tot =  binom (N, K);

  if (tot > nq)
    tot = nq;

  if (tot < BATCH_SIZE)
    tot = BATCH_SIZE;

#ifdef DEBUG_POLYSELECT
  fprintf (stderr, "# Info: n=%lu, k=%lu, (n,k)=%lu"
	   ", maxnq=%lu, nq=%lu\n", N, K, binom(N, K), nq, tot);
#endif

  i = 0;
  while ( i <= (tot-BATCH_SIZE) ) {

    l = i; // why do I use an extra l here?
    if (l == 0) {

      // enumerate first combination
      first_comb (K, idx_q);
      //print_comb (K, idx_q);
      q[l] = return_q_rq (SQ_R, idx_q, K, qqz[l], rqqz[l]);

      for (l = 1; l < BATCH_SIZE; l++) {
        next_comb (N, K, idx_q);
        q[l] = return_q_rq (SQ_R, idx_q, K, qqz[l], rqqz[l]);
      }
    }
    else {
      for (l = 0; l < BATCH_SIZE; l++) {
        next_comb (N, K, idx_q);
        q[l] = return_q_rq (SQ_R, idx_q, K, qqz[l], rqqz[l]);
      }
    }

#ifdef DEBUG_POLYSELECT
    unsigned long j;
    for (j = 0; j < BATCH_SIZE; j++)
      gmp_fprintf (stderr, "q: %lu, qq: %Zd, rqq: %Zd\n",
		   q[j], qqz[j], rqqz[j]);
#endif

    // collision batch
    gmp_collision_on_batch_sq (header, R, q, qqz, rqqz, BATCH_SIZE, c);
    i += BATCH_SIZE;
  }

  // tail batch
  for (l = 0; l < (tot % BATCH_SIZE); l++) {
    next_comb (N, K, idx_q);
    q[l] = return_q_rq (SQ_R, idx_q, K, qqz[l], rqqz[l]);

#ifdef DEBUG_POLYSELECT
    gmp_fprintf (stderr, "q: %lu, qq: %Zd, rqq: %Zd\n",
		 q[l], qqz[l], rqqz[l]);
#endif

  }

  gmp_collision_on_batch_sq (header, R, q, qqz, rqqz, tot % BATCH_SIZE, c);

  for (l = 0; l < BATCH_SIZE; l++) {
    mpz_clear (qqz[l]);
    mpz_clear (rqqz[l]);
  }
  free (qqz);
  free (rqqz);
  qroots_clear (SQ_R);
}

static void
newAlgo (mpz_t N, unsigned long d, unsigned long idx)
{
  mpz_t ad;
  unsigned long c = 0;
  header_t header;
  proots_t R;

  mpz_init_set_ui (ad, incr);
  mpz_mul_ui (ad, ad, idx);
  mpz_add (ad, ad, admin);
  header_init (header, N, d, ad);
  proots_init (R, lenPrimes);

  if (sizeof (unsigned long int) == 8) {
    shash_t H;
    shash_init (H, 4 * lenPrimes);
    c = collision_on_p (header, R, H);
    if (nq > 0)
      collision_on_sq (header, R, c, H);
    shash_clear (H);
  }
  else {
    c = gmp_collision_on_p (header, R);
    if (nq > 0)
      gmp_collision_on_sq (header, R, c);
  }

  proots_clear (R, lenPrimes);
  header_clear (header);
  mpz_clear (ad);
}

static void
declare_usage(param_list pl)
{
  param_list_decl_usage(pl, "degree", "(required, alias d) polynomial degree");
  param_list_decl_usage(pl, "n", "(required, alias N) input number");
  param_list_decl_usage(pl, "P", "(required) deg-1 coeff of g(x) has two prime factors in [P,2P]\n");

  param_list_decl_usage(pl, "admax", "maximal value for ad (+ 1)");
  param_list_decl_usage(pl, "admin", "minimal value for ad (default 0)");
  param_list_decl_usage(pl, "incr", "increment of ad (default 60)");
  param_list_decl_usage(pl, "maxtime", "stop the search after maxtime seconds");

  char str[200];
  snprintf (str, 200, "maximum number of special-q's considered\n"
            "               for each ad (default %d)", DEFAULT_NQ);
  param_list_decl_usage(pl, "nq", str);
  snprintf (str, 200, "number of polynomials kept (default %d)", KEEP);
  param_list_decl_usage(pl, "keep", str);
  snprintf (str, 200, "size-optimization effort (default %d)", SOPT_DEFAULT_EFFORT);
  param_list_decl_usage(pl, "sopteffort", str);
  param_list_decl_usage(pl, "s", str);
  param_list_decl_usage(pl, "t", "number of threads to use (default 1)");
  param_list_decl_usage(pl, "v", "verbose mode");
  param_list_decl_usage(pl, "q", "quiet mode");
  param_list_decl_usage(pl, "target_E", "target E-value\n");
  verbose_decl_usage(pl);
}

static void
usage (const char *argv, const char * missing, param_list pl)
{
  if (missing) {
    fprintf(stderr, "\nError: missing or invalid parameter \"-%s\"\n",
        missing);
  }
  param_list_print_usage(pl, argv, stderr);
  param_list_clear (pl);
  exit (EXIT_FAILURE);
}

int
main (int argc, char *argv[])
{
  char **argv0 = argv;
  double st0 = seconds (), wct0 = wct_seconds ();
  mpz_t N;
  unsigned int d = 0;
  unsigned long P = 0;
  int quiet = 0, nthreads = 1, st;
  unsigned long idx_max = 0; /* ad = admin+idx*incr, for 0 <= idx < idx_max */

  mpz_init (N);
  mpz_init (admin);
  mpz_init (admax);
  cado_poly_init (best_poly);
  cado_poly_init (curr_poly);
  data_init (data_opt_lognorm);
  data_init (data_exp_E);
  data_init (data_best_exp_E);
  data_init (data_beta);
  data_init (data_eta);
  data_init (raw_proj_alpha);
  data_init (opt_proj_alpha);

  /* read params */
  param_list pl;
  param_list_init (pl);

  declare_usage(pl);

  param_list_configure_switch (pl, "-v", &verbose);
  param_list_configure_switch (pl, "-q", &quiet);
  param_list_configure_alias(pl, "degree", "-d");
  param_list_configure_alias(pl, "incr", "-i");
  param_list_configure_alias(pl, "n", "-N");

  if (argc == 1)
    usage (argv0[0], NULL, pl);

  argv++, argc--;
  for ( ; argc; ) {
    if (param_list_update_cmdline (pl, &argc, &argv)) continue;
    fprintf (stderr, "Unhandled parameter %s\n", argv[0]);
    usage (argv0[0], NULL, pl);
  }

  /* size optimization effort that passed to size_optimization */
  param_list_parse_uint (pl, "sopteffort", &sopt_effort);

  param_list_parse_size_t (pl, "keep", &keep);
  /* initialize best norms */
  if (keep > 0) {
    best_opt_logmu = (double *) malloc(keep * sizeof(double));
    ASSERT_ALWAYS(best_opt_logmu != NULL);
    best_exp_E = (double *) malloc(keep * sizeof(double));
    ASSERT_ALWAYS(best_exp_E != NULL);
  } else {
    /* Allow keep == 0, say, if we want only timings. malloc() may or may not
       return a NULL pointer for a size argument of zero, so we do it
       ourselves. */
    best_opt_logmu = NULL;
    best_exp_E = NULL;
  }
  for (size_t i = 0; i < keep; i++)
    {
      best_opt_logmu[i] = LOGNORM_MAX; /* best logmu after size optimization */
      best_exp_E[i] = LOGNORM_MAX;     /* best logmu after rootsieve */
    }

  /* parse and check N in the first place */
  int have_n = param_list_parse_mpz (pl, "n", N);

  if (!have_n) {
    fprintf(stderr, "# Reading n from stdin\n");
    param_list_read_stream(pl, stdin, 0);
    have_n = param_list_parse_mpz(pl, "n", N);
  }

  if (!have_n) {
      fprintf(stderr, "No n defined ; sorry.\n");
      exit(1);
  }

  if (mpz_cmp_ui (N, 0) <= 0) usage(argv0[0], "n", pl);

  param_list_parse_ulong(pl, "P", &P);
  if (P == 0) usage(argv0[0], "P", pl);

  param_list_parse_int (pl, "t", &nthreads);
#ifdef HAVE_OPENMP
  omp_set_num_threads (nthreads);
#endif
  param_list_parse_ulong (pl, "nq", &nq);
  param_list_parse_uint (pl, "degree", &d);

  /* if no -admin is given, mpz_init did set it to 0, which is exactly
     what we want */
  param_list_parse_mpz (pl, "admin", admin);

  if (param_list_parse_mpz (pl, "admax", admax) == 0) /* no -admax */
    idx_max = ULONG_MAX;

  param_list_parse_ulong (pl, "incr", &incr);
  param_list_parse_double (pl, "maxtime", &maxtime);
  param_list_parse_double (pl, "target_E", &target_E);

  if (param_list_warn_unused(pl))
    usage (argv0[0], NULL, pl);

  /* print command line */
  verbose_interpret_parameters(pl);
  param_list_print_command_line (stdout, pl);

  /* check degree */
  if (d <= 0) usage(argv0[0], "degree", pl);

  /* maxtime and target_E are incompatible */
  if (maxtime != DBL_MAX && target_E != 0.0)
    {
      fprintf (stderr, "Options -maxtime and -target_E are incompatible\n");
      exit (1);
    }

  /* quiet mode */
  if (quiet == 1)
    verbose = -1;

  /* set cpoly */
  mpz_set (best_poly->n, N);
  mpz_set (curr_poly->n, N);
  best_poly->pols[ALG_SIDE]->deg = d;
  best_poly->pols[RAT_SIDE]->deg = 1;
  curr_poly->pols[ALG_SIDE]->deg = d;
  curr_poly->pols[RAT_SIDE]->deg = 1;

  /* initialize primes in [P,2*P] */
  double Pd;
  Pd = (double) P;
  if (Pd > (double) UINT_MAX) {
    fprintf (stderr, "Error, too large value of P\n");
    exit (1);
  }
  if (P <= (unsigned long) SPECIAL_Q[LEN_SPECIAL_Q - 2]) {
    fprintf (stderr, "Error, too small value of P, need P > %u\n",
             SPECIAL_Q[LEN_SPECIAL_Q - 2]);
    exit (1);
  }
  /* since for each prime p in [P,2P], we convert p^2 to int64_t, we need
     (2P)^2 < 2^63, thus P < 2^30.5 */
  /* XXX In fact there is a bug in collision_on_p which limit P to 2^30 */
  if (P >= 1073741824UL)
    {
      fprintf (stderr, "Error, too large value of P\n");
      exit (1);
    }

  st = milliseconds ();
  lenPrimes = initPrimes (P, &Primes);

  printf ("# Info: initializing %lu P primes took %lums, nq=%lu\n",
          lenPrimes, milliseconds () - st, nq);
  printf ( "# Info: estimated peak memory=%.2fMB (%d thread(s),"
           " batch %d inversions on SQ)\n",
           (double) (nthreads * (BATCH_SIZE * 2 + INIT_FACTOR) * lenPrimes
           * (sizeof(uint32_t) + sizeof(uint64_t)) / 1024 / 1024),
           nthreads, BATCH_SIZE);

  if (incr <= 0)
  {
    fprintf (stderr, "Error, incr should be positive\n");
    exit (1);
  }

  /* admin should be nonnegative */
  if (mpz_cmp_ui (admin, 0) < 0)
    {
      fprintf (stderr, "Error, admin should be nonnegative\n");
      exit (1);
    }

  /* if admin = 0, start from incr */
  if (mpz_cmp_ui (admin, 0) == 0)
    mpz_set_ui (admin, incr);

  /* admin should be a non-zero multiple of 'incr', since when the global
     [admin, admax] range is cut by cado-nfs.py between different workunits,
     some bounds might no longer be multiple of 'incr'. */
  mpz_add_ui (admin, admin, (incr - mpz_fdiv_ui (admin, incr)) % incr);

  if (idx_max == 0) /* admax was given */
    {
      /* ad = admin + idx * incr < admax
         thus idx_max = floor((admax-admin-1)/incr) + 1
                      = floor((admax-admin+incr-1)/incr) */
      mpz_t t;
      mpz_init_set (t, admax);
      mpz_sub (t, t, admin);
      mpz_add_ui (t, t, incr - 1);
      mpz_fdiv_q_ui (t, t, incr);
      if (mpz_fits_ulong_p (t))
        idx_max = mpz_get_ui (t);
      else
        idx_max = ULONG_MAX;
      mpz_clear (t);
    }

#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
  for (unsigned long idx = 0; idx < idx_max; idx ++)
    {
      newAlgo (N, d, idx);
      if (verbose > 0)
#ifdef HAVE_OPENMP
#pragma omp critical
#endif
        {
          printf ("# thread %d completed ad=%.0f at time=%.2fs\n",
                  omp_get_thread_num (), get_ad_double (idx),
                  wct_seconds () - wct0);
          fflush (stdout);
        }
    }

  /* finishing up statistics */
  if (verbose >= 0)
    {
      potential_collisions *= expected_collisions (Primes[lenPrimes - 1]);
      printf ("# Stat: potential collisions=%1.2f (%1.2e/s)\n",
              potential_collisions, 1000.0 * potential_collisions
              / (double) milliseconds ());
      if (collisions > 0)
        {
          double rawmean = aver_raw_lognorm / collisions;

          printf ("# Stat: raw lognorm (nr/min/av/max/std): %lu/%1.2f/%1.2f/%1.2f/%1.2f\n",
                  collisions, min_raw_lognorm, rawmean, max_raw_lognorm,
                  sqrt (var_raw_lognorm / collisions - rawmean * rawmean));
          printf ("# Stat: raw proj. alpha (nr/min/av/max/std): %lu/%1.3f/%1.3f/%1.3f/%1.3f\n",
                  collisions, raw_proj_alpha->min, data_mean (raw_proj_alpha), raw_proj_alpha->max, sqrt (data_var (raw_proj_alpha)));
	  printf ("# Stat: discarded %lu polynomials because f[d]*f[d-2] > 0\n",
		  discarded1);
          if (collisions_good > 0)
            {
              double mean = data_mean (data_opt_lognorm);
              double Emean = data_mean (data_exp_E);
              printf ("# Stat: optimized lognorm (nr/min/av/max/std): %lu/%1.2f/%1.2f/%1.2f/%1.2f\n",
                      collisions_good, data_opt_lognorm->min, mean, data_opt_lognorm->max,
                      sqrt (data_var (data_opt_lognorm)));
              printf ("# Stat: opt proj. alpha (nr/min/av/max/std): %lu/%1.3f/%1.3f/%1.3f/%1.3f\n",
                  collisions_good, opt_proj_alpha->min, data_mean (opt_proj_alpha), opt_proj_alpha->max, sqrt (data_var (opt_proj_alpha)));
              /* the exp_E statistics can be used as follows: if the mean is
                 m and the standard deviation s, then assuming a normal
                 distribution, the minimum order statistic for K polynomials
                 is given by:
                 m - s*(sqrt(2log(K))-(log(log(K))+1.377)/(2*sqrt(2log(K))))
              */
	      printf ("# Stat: discarded %lu polynomials because f[d-1]*f[d-3] > 0\n",
		  discarded2);
              printf ("# Stat: exp_E (nr/min/av/max/std): %lu/%1.2f/%1.2f/%1.2f/%1.3f\n",
                      collisions_good, data_exp_E->min, Emean,
                      data_exp_E->max, sqrt (data_var (data_exp_E)));
            }
        }
    }

  printf ("# Stat: tried %lu ad-value(s), %lu size-optimized polynomials, kept %lu\n",
          idx_max, opt_found, collisions_good);

  clearPrimes (&Primes);

  /* print best keep values of logmu */
  if (collisions_good > 0)
    {
      printf ("# Stat: best exp_E after size optimization:");
      for (size_t i = 0; i < keep; i++)
        if (best_exp_E[i] < LOGNORM_MAX)
          printf (" %1.2f", best_exp_E[i]);
      printf ("\n");
    }

  /* print total time (this gets parsed by the scripts) */
  printf ("# Stat: total phase took %.2fs\n", seconds () - st0);
#ifndef HAVE_RUSAGE_THREAD /* optimize_time is correct only if RUSAGE_THREAD
                              works or in mono-thread mode */
  if (nthreads == 1)
#endif
    printf ("# Stat: size-optimization took %.2fs\n", optimize_time);

  mpz_clear (N);
  mpz_clear (admin);
  mpz_clear (admax);
  cado_poly_clear (best_poly);
  cado_poly_clear (curr_poly);
  free(best_opt_logmu);
  free(best_exp_E);
  param_list_clear (pl);
  data_clear (data_opt_lognorm);
  data_clear (data_exp_E);
  data_clear (data_best_exp_E);
  data_clear (data_beta);
  data_clear (data_eta);
  data_clear (raw_proj_alpha);
  data_clear (opt_proj_alpha);

  return 0;
}
