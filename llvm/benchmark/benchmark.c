
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <dbrew.h>
#include <dbrew-llvm.h>

#include "kernels.h"
#include "timer.h"


typedef void(*StencilLineFunction)(void*, double* restrict, double* restrict, uint64_t, StencilFunction);
typedef void(*StencilMatrixFunction)(void*, double* restrict, double* restrict, uint64_t, StencilFunction);
typedef void(*ParameterSetupFunction)(void**, void**, void**);
typedef void(*TestFunction)(void*, StencilFunction, double*, double*);


Stencil s5 = {4, {{-1,0,0.25},{1,0,0.25},{0,-1,0.25},{0,1,0.25}}};

SortedStencil s5s = {1, {{0.25,4,&(s5.p[0])}}};


static void
compute_jacobi(void* restrict a, StencilFunction fn, double* restrict b, double* restrict c)
{
    uint64_t i, j;
    for (uint64_t iter = 0; iter < 1000; iter = iter + 1)
    {
        double* temp = c;
        c = b;
        b = temp;

        for (i = 1; i < STENCIL_N; i = i + 1)
            for (j = 1; j < STENCIL_N; j = j + 1)
                fn(a, b, c, STENCIL_INDEX(j, i));
    }
}

static void
compute_jacobi_line(void* restrict a, StencilLineFunction fn, double* restrict b, double* restrict c)
{
    uint64_t i;
    for (uint64_t iter = 0; iter < 1000; iter = iter + 1)
    {
        double* temp = c;
        c = b;
        b = temp;

        for (i = 1; i < STENCIL_N; i = i + 1)
            fn(a, b, c, STENCIL_INDEX(0, i), NULL);
    }
}

static void
compute_jacobi_matrix(void* restrict a, StencilMatrixFunction fn, double* restrict b, double* restrict c)
{
    for (uint64_t iter = 0; iter < 1000; iter = iter + 1)
    {
        double* temp = c;
        c = b;
        b = temp;

        fn(a, b, c, 0, NULL);
    }
}


static
void
init_matrix(double** matrixIn, double** matrixOut)
{
    double* b = malloc(sizeof(double) * (STENCIL_N + 1) * (STENCIL_N + 1));
    for (int i = 0; i <= STENCIL_N; i++) {
        for (int j = 0; j <= STENCIL_N; j++) {
            int index = STENCIL_INDEX(j, i);
            if (i == 0) // First Row
                b[index] = 1.0 - (j * 1.0 / STENCIL_N);
            else if (i == STENCIL_N) // Last Row
                b[index] = j * 1.0 / STENCIL_N;
            else if (j == 0) // First Column
                b[index] = 1.0 - (i * 1.0 / STENCIL_N);
            else if (j == STENCIL_N) // Last Column
                b[index] = i * 1.0 / STENCIL_N;
            else
                b[index] = 0;
        }
    }
    *matrixOut = malloc(sizeof(double) * (STENCIL_N + 1) * (STENCIL_N + 1));
    memcpy(*matrixOut, b, sizeof(double) * (STENCIL_N + 1) * (STENCIL_N + 1));

    *matrixIn = b;
}

static void
print_matrix(double* b)
{
    printf("Matrix:\n");

    for (int y = 0; y < 9; y++)
    {
        for (int x = 0; x < 9; x++)
        {
            int index = STENCIL_INDEX(x * (STENCIL_INTERLINES + 1), y * (STENCIL_INTERLINES + 1));
            printf("%7.4f", b[index]);
        }
        printf("\n");
    }
}


enum BenchmarkMode {
    BENCHMARK_PLAIN,
    BENCHMARK_DBREW,
    BENCHMARK_LLVM,
    BENCHMARK_LLVM_FIXED,
    BENCHMARK_DBREW_LLVM,
    BENCHMARK_DBREW_LLVM_TWICE,
};

typedef enum BenchmarkMode BenchmarkMode;

enum StencilGranularity {
    GRAN_ELEM = 0,
    GRAN_LINE = 1,
    GRAN_MATRIX = 2,
};

typedef enum StencilGranularity StencilGranularity; 

struct BenchmarkArgs {
    BenchmarkMode mode;
    StencilGranularity granularity;
    size_t runCount;
    bool decodeGenerated;
};

typedef struct BenchmarkArgs BenchmarkArgs;

struct BenchmarkStencilConfig {
    void* funcs[3];
    void* data;
};

typedef struct BenchmarkStencilConfig BenchmarkStencilConfig;

static const BenchmarkStencilConfig benchmarkConfigs[] = {
    { { (void*) stencil_element_native, (void*) stencil_line_native, (void*) stencil_matrix_native }, NULL },
    { { (void*) stencil_element_struct, (void*) stencil_line_struct, (void*) stencil_matrix_struct }, &s5 },
    { { (void*) stencil_element_sorted_struct, (void*) stencil_line_sorted_struct, (void*) stencil_matrix_sorted_struct }, &s5s },
};

static
Rewriter*
benchmark_init_dbrew(StencilGranularity granularity)
{
    Rewriter* r = dbrew_new();
    dbrew_verbose(r, false, false, false);
    dbrew_optverbose(r, false);
    dbrew_set_decoding_capacity(r, 100000, 100);
    dbrew_set_capture_capacity(r, 100000, 100, 10000);
    
    switch (granularity)
    {
        case GRAN_ELEM:
            dbrew_set_function(r, (uintptr_t) stencil_element_dbrew);
            break;
        case GRAN_LINE:
            dbrew_set_function(r, (uintptr_t) stencil_line_dbrew);
            break;
        case GRAN_MATRIX:
            dbrew_set_function(r, (uintptr_t) stencil_matrix_dbrew);
            break;
    }

    dbrew_config_staticpar(r, 0);
    dbrew_config_staticpar(r, 4);
    dbrew_config_parcount(r, 5);
    dbrew_config_force_unknown(r, 0);

    return r;
}

static
void
benchmark_run2(size_t configIndex, const BenchmarkArgs* args, const BenchmarkStencilConfig* config)
{
    void* arg0 = config->data;

    double* arg1;
    double* arg2;
    init_matrix(&arg1, &arg2);

    LLConfig llconfig = {
        .name = "test",
        .stackSize = 128,
        .signature = 0211114,
    };

    LLState* state = NULL;
    LLFunction* llfn = NULL;
    Rewriter* r = NULL;

    uintptr_t baseFunction = (uintptr_t) config->funcs[args->granularity];
    uintptr_t processedFunction;

    JTimer timerCompile = {0}, timerRun = {0};

    __asm__ volatile("" ::: "memory");
    JTimerCont(&timerCompile);
    __asm__ volatile("" ::: "memory");

    if (args->mode != BENCHMARK_PLAIN)
    {
        r = benchmark_init_dbrew(args->granularity);
        dbrew_optverbose(r, args->decodeGenerated);
    }

    if (args->mode == BENCHMARK_LLVM || args->mode == BENCHMARK_LLVM_FIXED || args->mode == BENCHMARK_DBREW_LLVM_TWICE)
    {
        state = ll_engine_init();
        ll_engine_enable_fast_math(state, true);
    }

    switch (args->mode)
    {
        case BENCHMARK_PLAIN:
            processedFunction = baseFunction;
            break;

        case BENCHMARK_DBREW:
            processedFunction = dbrew_rewrite(r, arg0, arg1, arg2, 20, config->funcs[0]);
            break;

        case BENCHMARK_DBREW_LLVM:
            processedFunction = dbrew_llvm_rewrite(r, arg0, arg1, arg2, 20, config->funcs[0]);
            break;

        case BENCHMARK_LLVM:
            llfn = ll_decode_function(baseFunction, (DecodeFunc) dbrew_decode, r, &llconfig, state);
            assert(llfn != NULL);
            break;

        case BENCHMARK_LLVM_FIXED:
            llfn = ll_decode_function(baseFunction, (DecodeFunc) dbrew_decode, r, &llconfig, state);
            assert(llfn != NULL);

            if (arg0 != NULL)
                llfn = ll_function_specialize(llfn, 0, (uintptr_t) arg0, 0x100, state);
            break;

        case BENCHMARK_DBREW_LLVM_TWICE:
            processedFunction = dbrew_llvm_rewrite(r, arg0, arg1, arg2, 20, config->funcs[0]);

            llfn = ll_decode_function(processedFunction, (DecodeFunc) dbrew_decode, r, &llconfig, state);
            assert(llfn != NULL);
            break;

        default:
            assert(0);
    }

    if (args->mode == BENCHMARK_LLVM || args->mode == BENCHMARK_LLVM_FIXED || args->mode == BENCHMARK_DBREW_LLVM_TWICE)
    {
        ll_engine_optimize(state, 3);
        processedFunction = (uintptr_t) ll_function_get_pointer(llfn, state);

        if (args->decodeGenerated)
            ll_engine_dump(state);
    }

    __asm__ volatile("" ::: "memory");
    JTimerStop(&timerCompile);
    __asm__ volatile("" ::: "memory");

    if (args->decodeGenerated)
    {
        if (state == NULL)
            state = ll_engine_init();

        if (r == NULL)
            r = benchmark_init_dbrew(true);

        // Print out decoded assembly.
        dbrew_verbose(r, true, false, false);
        ll_decode_function((uintptr_t) processedFunction, (DecodeFunc) dbrew_decode, r, &llconfig, state);
    }

    __asm__ volatile("" ::: "memory");
    JTimerCont(&timerRun);
    __asm__ volatile("" ::: "memory");
    switch (args->granularity)
    {
        case GRAN_ELEM:
            for (size_t runs = 0; runs < args->runCount; runs++)
                compute_jacobi(arg0, (StencilFunction) processedFunction, arg1, arg2);
            break;
        case GRAN_LINE:
            for (size_t runs = 0; runs < args->runCount; runs++)
                compute_jacobi_line(arg0, (StencilLineFunction) processedFunction, arg1, arg2);
            break;
        case GRAN_MATRIX:
            for (size_t runs = 0; runs < args->runCount; runs++)
                compute_jacobi_matrix(arg0, (StencilMatrixFunction) processedFunction, arg1, arg2);
            break;
    }
    __asm__ volatile("" ::: "memory");
    JTimerStop(&timerRun);
    __asm__ volatile("" ::: "memory");

    printf("matrix(n-1,n-1) = %f\n", arg2[STENCIL_INDEX(STENCIL_N-1, STENCIL_N-1)]);
    printf("transmode=%d;gran=%u;datatype=%zu;n=%d;ctime=%f;rtime=%f\n", args->mode, args->granularity, configIndex, STENCIL_N, JTimerRead(&timerCompile), JTimerRead(&timerRun));

    free(arg1);
    free(arg2);

    if (state != NULL)
        ll_engine_dispose(state);

    if (r != NULL)
        dbrew_free(r);
}

int
main(int argc, char** argv)
{
    if (argc < 6) {
        printf("Usage: %s [transmode] [granularity] [datatype] [compiles] [runs per compile] ([decode generated])\n", argv[0]);
        return 1;
    }

    size_t configIndex = atoi(argv[3]);
    configIndex = configIndex % (sizeof(benchmarkConfigs) / sizeof(BenchmarkStencilConfig));

    bool decodeGenerated = false;
    if (argc >= 7)
        decodeGenerated = atoi(argv[6]) != 0;

    BenchmarkArgs args = {
        .mode = atoi(argv[1]),
        .granularity = atoi(argv[2]) % 3,
        .runCount = atoi(argv[5]),
        .decodeGenerated = decodeGenerated,
    };

    size_t iterationCount = atoi(argv[4]);

    const BenchmarkStencilConfig* config = &benchmarkConfigs[configIndex];

    for (size_t i = 0; i < iterationCount; i++)
    {
        benchmark_run2(configIndex, &args, config);
        args.decodeGenerated = false;
    }


    return 0;
}
