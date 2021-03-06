/*
 * Example for DBrew vectorization API
 */

#include "dbrew.h"

#include <stdio.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

typedef void (*vcopy_t)(double*, double*, int);
typedef void (*vadd_t)(double*, double*, double*, int);

// vcopy

double copy_kernel(double v)
{
    return v;
}

void vcopy(double* dst, double* src, int n)
{
#if 0
    dbrew_apply4_R8V8(copy_kernel, dst, src);
#else
    while(n>3) {
        dbrew_apply4_R8V8(copy_kernel, dst, src);
        dst += 4;
        src += 4;
        n -= 4;
    }
    while(n>0) {
        *dst++ = *src++;
        n--;
    }
#endif
}

__attribute__ ((noinline))
void naive_vcopy(double* dst, double* src, int n)
{
    int i;
    for(i = 0; i < n; i++)
        dst[i] = src[i];
}


// vadd

double add_kernel(double v1, double v2)
{
    return v1 + v2;
}

void vadd(double* dst, double* src1, double* src2, int n)
{
#if 0
    dbrew_apply4_R8V8V8(add_kernel, dst, src1, src2);
#else
    while(n>3) {
        dbrew_apply4_R8V8V8(add_kernel, dst, src1, src2);
        dst += 4;
        src1 += 4;
        src2 += 4;
        n -= 4;
    }
    while(n>0) {
        *dst++ = add_kernel(*src1++, *src2++);
        n--;
    }
#endif
}

__attribute__ ((noinline))
void naive_vadd(double* dst, double* src1, double* src2, int n)
{
    int i;
    for(i = 0; i < n; i++)
        dst[i] = src1[i] + src2[i];
}


// vjacobi_1d

double jacobi_1d_kernel(double* v)
{
    return 0.5 * (v[-1] + v[1]);
}

void vjacobi_1d(double* dst, double* src, int n)
{
#if 0
    dbrew_apply4_R8VP8(jacobi_1d_kernel, dst, src);
#else
    while(n>3) {
        dbrew_apply4_R8P8(jacobi_1d_kernel, dst, src);
        dst += 4;
        src += 4;
        n -= 4;
    }
    while(n>0) {
        *dst++ = jacobi_1d_kernel(src++);
        n--;
    }
#endif
}

__attribute__ ((noinline))
void naive_vjacobi_1d(double* dst, double* src, int n)
{
    int i;
    for(i = 0; i < n; i++)
        dst[i] = 0.5 * (src[i-1] + src[i+1]);
}


double wtime()
{
  struct timeval tv;
  gettimeofday(&tv, 0);

  return tv.tv_sec+1e-6*tv.tv_usec;
}

// for decoding code generated by rewriting
void decode_func(Rewriter* r, const char* n)
{
    Rewriter* rr = dbrew_new();
    uint64_t func = dbrew_generated_code(r);
    int size = dbrew_generated_size(r);
    dbrew_config_function_setname(rr, func, n);
    dbrew_config_function_setsize(rr, func, size);
    dbrew_decode_print(rr, func, size);
    dbrew_free(rr);
}


int main(int argc, char* argv[])
{
    double t1, t2, t3, t4, t5;
    double sum1, sum2, sum3, sum4;
    int arg = 1, len = 0, iters = 0, verb = 0, run = 1;
    int do_vcopy = 1, do_vadd = 1, do_vjacobi = 1;
    while(argc>arg) {
        if      (strcmp(argv[arg],"-v")==0)  verb++;
        else if (strcmp(argv[arg],"-vv")==0) verb+=2;
        else if (strcmp(argv[arg],"-n")==0)  run = 0;
        else if (strcmp(argv[arg],"-c")==0)  do_vadd = 0,  do_vjacobi = 0;
        else if (strcmp(argv[arg],"-a")==0)  do_vcopy = 0, do_vjacobi = 0;
        else if (strcmp(argv[arg],"-j")==0)  do_vcopy = 0, do_vadd = 0;
        else
            break;
        arg++;
    }
    if (argc>arg) { len   = atoi(argv[arg]); arg++; }
    if (argc>arg) { iters = atoi(argv[arg]); arg++; }
    if (len == 0) len = 10000;
    if (iters == 0) iters = 20;
    len = len * 1000;

    printf("Alloc/init 3 double arrays of length %d ...\n", len);
    double* a = (double*) malloc(len * sizeof(double));
    double* b = (double*) malloc(len * sizeof(double));
    double* c = (double*) malloc(len * sizeof(double));
    for(int i = 0; i<len; i++) {
        a[i] = 1.0;
        b[i] = (double) (i % 20);
        c[i] = 3.0;
    }

    // Generate vectorized variants & run against naive/original

#if __AVX__
    bool do32 = true;
#else
    bool do32 = false;
#endif

    // vcopy

    if (do_vcopy) {
        vcopy_t vcopy16, vcopy32;

        Rewriter* rc16 = dbrew_new();
        if (verb>1) dbrew_verbose(rc16, true, true, true);
        dbrew_set_function(rc16, (uint64_t) vcopy);
        dbrew_config_parcount(rc16, 3);
        dbrew_config_force_unknown(rc16, 0);
        dbrew_set_vectorsize(rc16, 16);
        vcopy16 = (vcopy_t) dbrew_rewrite(rc16, a, b, len);
        if (verb) decode_func(rc16, "vcopy16");

        if (do32) {
            Rewriter* rc32 = dbrew_new();
            if (verb>1) dbrew_verbose(rc32, true, true, true);
            dbrew_set_function(rc32, (uint64_t) vcopy);
            dbrew_config_parcount(rc32, 3);
            dbrew_config_force_unknown(rc32, 0);
            dbrew_set_vectorsize(rc32, 32);
            vcopy32 = (vcopy_t) dbrew_rewrite(rc32, a, b, len);
            if (verb) decode_func(rc32, "vcopy32");
        }

        printf("Running %d iterations of vcopy ...\n", iters);
        t1 = wtime();
        for(int iter = 0; iter < iters; iter++)
            naive_vcopy(a, b, len);
        t2 = wtime();
        for(int iter = 0; iter < iters; iter++)
            vcopy(a, b, len);
        t3 = wtime();
        if (run)
            for(int iter = 0; iter < iters; iter++)
                vcopy16(a, b, len);
        t4 = wtime();
        if (do32 && run)
            for(int iter = 0; iter < iters; iter++)
                vcopy32(a, b, len);
        t5 = wtime();
        printf("  naive: %.3f s, un-rewritten: %.3f s, rewritten-16: %.3f s",
               t2-t1, t3-t2, t4-t3);
        if (do32)
            printf(", rewritten-32: %.3f s", t5-t4);
        printf("\n");
    }


    // vadd

    if (do_vadd) {
        vadd_t vadd16, vadd32;

        Rewriter* ra16 = dbrew_new();
        if (verb>1) dbrew_verbose(ra16, true, true, true);
        dbrew_set_function(ra16, (uint64_t) vadd);
        dbrew_config_parcount(ra16, 4);
        dbrew_config_force_unknown(ra16, 0);
        dbrew_set_vectorsize(ra16, 16);
        vadd16 = (vadd_t) dbrew_rewrite(ra16, a, b, c, len);
        if (verb) decode_func(ra16, "vadd16");

        if (do32) {
            Rewriter* ra32 = dbrew_new();
            if (verb>1) dbrew_verbose(ra32, true, true, true);
            dbrew_set_function(ra32, (uint64_t) vadd);
            dbrew_config_parcount(ra32, 4);
            dbrew_config_force_unknown(ra32, 0);
            dbrew_set_vectorsize(ra32, 32);
            vadd32 = (vadd_t) dbrew_rewrite(ra32, a, b, c, len);
            if (verb) decode_func(ra32, "vadd32");
        }

        sum1 = 0.0, sum2 = 0.0, sum3 = 0.0, sum4 = 0.0;
        printf("Running %d iterations of vadd ...\n", iters);
        t1 = wtime();
        for(int iter = 0; iter < iters; iter++)
            naive_vadd(a, b, c, len);
        for(int i = 0; i < len; i++) sum1 += a[i];
        t2 = wtime();
        for(int iter = 0; iter < iters; iter++)
            vadd(a, b, c, len);
        for(int i = 0; i < len; i++) sum2 += a[i];
        t3 = wtime();
        if (run)
            for(int iter = 0; iter < iters; iter++)
                vadd16(a, b, c, len);
        for(int i = 0; i < len; i++) sum3 += a[i];
        t4 = wtime();
        if (do32 && run)
            for(int iter = 0; iter < iters; iter++)
                vadd32(a, b, c, len);
        for(int i = 0; i < len; i++) sum4 += a[i];
        t5 = wtime();

        printf("  naive: %.3f s, un-rewritten: %.3f s, rewritten-16: %.3f s",
               t2-t1, t3-t2, t4-t3);
        if (do32)
            printf(", rewritten-32: %.3f s", t5-t4);
        printf("\n");
        printf("  sum naive: %f, sum rewritten-16: %f, sum rewritten-16: %f\n",
               sum1, sum3, sum4);
    }


    // vjacobi_1d

    if (do_vjacobi) {
        vcopy_t vjacobi_1d16, vjacobi_1d32;

        Rewriter* rj16 = dbrew_new();
        if (verb>1) dbrew_verbose(rj16, true, true, true);
        dbrew_set_function(rj16, (uint64_t) vjacobi_1d);
        dbrew_config_parcount(rj16, 3);
        dbrew_config_force_unknown(rj16, 0);
        dbrew_set_vectorsize(rj16, 16);
        vjacobi_1d16 = (vcopy_t) dbrew_rewrite(rj16, a, b, len);
        if (verb) decode_func(rj16, "vjacobi_1d16");

        if (do32) {
            Rewriter* rj32 = dbrew_new();
            if (verb>1) dbrew_verbose(rj32, true, true, true);
            dbrew_set_function(rj32, (uint64_t) vjacobi_1d);
            dbrew_config_parcount(rj32, 3);
            dbrew_config_force_unknown(rj32, 0);
            dbrew_set_vectorsize(rj32, 32);
            vjacobi_1d32 = (vcopy_t) dbrew_rewrite(rj32, a, b, len);
            if (verb) decode_func(rj32, "vjacobi_1d32");
        }

        sum1 = 0.0, sum2 = 0.0, sum3 = 0.0, sum4 = 0.0;
        printf("Running %d iterations of vjacobi_1d ...\n", iters);
        t1 = wtime();
        for(int iter = 0; iter < iters; iter++)
            naive_vjacobi_1d(a+1, b+1, len-2);
        for(int i = 0; i < len; i++) sum1 += a[i];
        t2 = wtime();
        for(int iter = 0; iter < iters; iter++)
            vjacobi_1d(a+1, b+1, len-2);
        for(int i = 0; i < len; i++) sum2 += a[i];
        t3 = wtime();
        if (run)
            for(int iter = 0; iter < iters; iter++)
                vjacobi_1d16(a+1, b+1, len-2);
        for(int i = 0; i < len; i++) sum3 += a[i];
        t4 = wtime();
        if (do32 && run)
            for(int iter = 0; iter < iters; iter++)
                vjacobi_1d32(a+1, b+1, len-2);
        for(int i = 0; i < len; i++) sum4 += a[i];
        t5 = wtime();
        printf("  naive: %.3f s, un-rewritten: %.3f s, rewritten-16: %.3f s",
               t2-t1, t3-t2, t4-t3);
        if (do32)
            printf(", rewritten-32: %.3f s", t5-t4);
        printf("\n");
        printf("  sum naive: %f, sum rewritten-16: %f, sum rewritten-16: %f\n",
               sum1, sum3, sum4);
    }
}
