/* Xcvhwlp: a short loop body (loop end fits the 5 bit uimmS of cv.setupi)
   combined with a constant trip count that does not fit the 12 bit uimmL
   of cv.setupi must be set up with cv.setup, not ICE.  */
/* { dg-do compile } */
/* { dg-skip-if "" { *-*-* } {"-O0" "-O1" "-Os" "-Og" "-O3" "-Oz" "-flto"} } */
/* { dg-options "-march=rv32imc_xcvhwlp -mabi=ilp32" } */

extern volatile short *dst;
extern const short src[6144];

void
copy (void)
{
  volatile short *d = dst;
  #pragma GCC unroll 1
  for (int i = 0; i < 6144; i++)
    d[i] = src[i];
}

/* cv.setupi cannot hold a count of 6144, so the compiler must emit cv.setup. */
/* { dg-final { scan-assembler "cv\\.setup\\s" } } */
/* { dg-final { scan-assembler-not "cv\\.setupi" } } */
