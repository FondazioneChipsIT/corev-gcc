/* { dg-do compile } */
/* { dg-require-effective-target cv_mem } */
/* { dg-options "-march=rv32ic_xcvmem -mabi=ilp32 -fno-unroll-loops" } */
/* We don't generate for some optimization levels because:
 * (1) O0 does not generate any post-increment pattern;
 * (2) Og, Osm and Oz generate register-register patterns. */
/* { dg-skip-if "" { *-*-* } { "-O0" "-Og" "-Os" "-Oz"} } */
/*
 * Test that USE_LOAD_POST_INCREMENT / USE_STORE_POST_INCREMENT causes ivopts
 * to prefer pointer IVs over integer-index IVs when two arrays are accessed
 * in the same loop, resulting in post-increment loads AND stores rather than
 * register-offset forms with a separate addi.
 */
void
scale_si (int * __restrict__ dst, const int * __restrict__ src, int n, int k)
{
  for (int i = 0; i < n; i++)
    dst[i] = src[i] * k;
}

void
scale_hi (short * __restrict__ dst, const short * __restrict__ src, int n, short k)
{
  for (int i = 0; i < n; i++)
    dst[i] = src[i] * k;
}

void
scale_qi (signed char * __restrict__ dst, const signed char * __restrict__ src,
          int n, signed char k)
{
  for (int i = 0; i < n; i++)
    dst[i] = src[i] * k;
}

/* { dg-final { scan-assembler-times "cv\\.lw\t\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\),\\(\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\)\\),4" 1 } } */
/* { dg-final { scan-assembler-times "cv\\.sw\t\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\),\\(\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\)\\),4" 1 } } */
/* { dg-final { scan-assembler-times "cv\\.lh\t\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\),\\(\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\)\\),2" 1 } } */
/* { dg-final { scan-assembler-times "cv\\.sh\t\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\),\\(\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\)\\),2" 1 } } */
/* { dg-final { scan-assembler-times "cv\\.lb\t\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\),\\(\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\)\\),1" 1 } } */
/* { dg-final { scan-assembler-times "cv\\.sb\t\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\),\\(\(\?\:t\[0-6\]\|a\[0-7\]\|s\[0-11\]\)\\),1" 1 } } */
