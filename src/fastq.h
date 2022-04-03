#ifndef FASTQ_H
#define FASTQ_H

#include "utils.h"
#include "dict.h"
#include<zlib.h>
#include "htslib/kstring.h"

struct qc_report {
    uint64_t all_fragments;
    uint64_t qc_failed;
    uint64_t unknown_barcodes;
};

#define FQ_PASS     0
#define FQ_QC_FAIL  1
#define FQ_BC_FAIL  2
#define FQ_DUP      3

#define FQ_FLAG_PASS          0
#define FQ_FLAG_BC_EXACTMATCH 1
#define FQ_FLAG_BC_FAILURE    2
#define FQ_FLAG_READ_QUAL     3
#define FQ_FLAG_SAMPLE_FAIL   4

struct bseq {
    int flag; // flag for skip reasons
    // read 1
    kstring_t n0;
    kstring_t s0, q0;
    // read 2
    kstring_t s1, q1;
    void *data; // extend data, should be freed manually
};

struct bseq_pool {
    int n, m;
    struct bseq *s;
    int force_fasta;
    void *opts; // used to point thread safe structure
};

struct fastq_handler {
    int n_file;    
    int curr; // curr file
    char **read_1;
    char **read_2;
    gzFile r1;
    gzFile r2;
    void *k1;
    void *k2;
    int smart_pair;
    int chunk_size;
    int closed;
};

#define FH_SE 1
#define FH_PE 2
#define FH_SMART_PAIR 3
#define FH_NOT_ALLOC 4
#define FH_NOT_INIT 5

void bseq_destroy(struct bseq*);

extern int check_name(char *s1, char *s2);

struct bseq_pool *bseq_pool_init0();
struct bseq_pool *bseq_pool_init(int size);

void bseq_pool_clean(struct bseq_pool *p);

void bseq_pool_destroy(struct bseq_pool *p);

// fastq handler must be inited before call fastq_read
void *fastq_read(void *h, void *opts);

extern struct fastq_handler *fastq_handler_init(const char *r1, const char *r2, int smart, int chunk_size);

extern int fastq_handler_state(struct fastq_handler*);

extern void fastq_handler_destory(struct fastq_handler *h);
extern void bseq_pool_push(struct bseq *b, struct bseq_pool *p);

extern int bseq_pool_dedup(struct bseq_pool *p);
extern size_t hamming_n(const char *a, const size_t length, const char *b, const size_t bLength);
extern size_t levenshtein_n(const char *a, const size_t length, const char *b, const size_t bLength);


void bseq_pool_write_fp(struct bseq_pool *p, FILE *fp);
void bseq_pool_write_file(struct bseq_pool *p, const char *fn);

// cache 
struct bseq_pool *bseq_pool_cache_fastq(FILE *fp, int n);
struct bseq_pool *bseq_pool_cache_fasta(FILE *fp, int n);
struct bseq_pool *bseq_pool_cache_fp(FILE *fp, int n);
struct bseq_pool *bseq_pool_cache_file(const char *fn);

struct bseq *fastq_read_one(struct fastq_handler *fastq);

struct bseq_pool *fastq_read_block(struct fastq_handler *fastq, struct dict *tags);

#endif
