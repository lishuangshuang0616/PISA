#include "utils.h"
#include "htslib/khash.h"
#include "htslib/kstring.h"
#include "sim_search.h"

#define BASE_TERM 0x0
#define BASE_A  0x1
#define BASE_C  0x2
#define BASE_G  0x3
#define BASE_T  0x4
#define BASE_N  0x5

//static int kmer_min = 5;
static int kmer_max = 21;

static int kmer_size = 5;

typedef struct ss_idx {
    int *idx;
    int n,m;
} sidx_t;

KHASH_MAP_INIT_INT64(ss64, int)
KHASH_MAP_INIT_INT(ss32, sidx_t)

typedef kh_ss64_t hash64_t;
typedef kh_ss32_t hash32_t;

struct similarity_search_aux {
    hash64_t *d0;
    hash32_t *d1; 
    uint64_t *cs; // compact sequence    
    int n, m;
};

uint8_t encode_base(char c)
{
    switch (c) {
        case 'A':
        case 'a':
            return BASE_A;
        case 'C':
        case 'c':
            return BASE_C;
        case 'G':
        case 'g':
            return BASE_G;
        case 'T':
        case 't':
            return BASE_T;
        case 'N':
        case 'n':
            return BASE_N;
        case '\0':
            return 0x0;
        default:
            error("Try to encode a non DNA sequence ? %c", c);
    }
}
// not safe for Ns
uint32_t enc32(char *s, int l)
{
    int len = strlen(s);
    if (len < l) error("Try to encode a truncated sequence ?");
    if (len > 16) error("Only support to encode sequence shorter than 16nt.");
    uint64_t q = 0;
    int i;
    for (i = 0; i < l; ++i)
        q = q<<3 | (encode_base(s[i]) & 0x7);
    return q;

}
// not safe for Ns
uint64_t enc64(char *s)
{
    int l = strlen(s);
    if (l > 32) error("Only support to encode sequence not longer than 32nt.");
    uint32_t q = 0;
    int i;
    for (i = 0; i < l; ++i)
        q = q<<3 | (encode_base(s[i]) & 0x7);
    return q;
}

char *decode64(uint64_t q)
{
    char c[23];
    memset(c, 0, 23);
    int i = 21;
    for (;;) {
        uint8_t x = q & 0x7;
        if (x == 0x1) c[i] = 'A'; // kputc('A', &str);
        else if (x == 0x2) c[i] = 'C'; // kputc('C', &str);
        else if (x == 0x3) c[i] = 'G'; // kputc('G', &str);
        else if (x == 0x4) c[i] = 'T'; // kputc('T', &str);
        else if (x == 0x5) c[i] = 'N';
        else break;
        i--;
        q = q>>3;
    }
    kstring_t str = {0,0,0};
    kputs(c+i+1, &str);
    return str.s;
}
char *decode32(uint32_t q)
{
    char c[11];
    memset(c, 0, 11);
    int i = 9;
    for (;;) {
        uint8_t x = q & 0x7;
        if (x == 0x1) c[i] = 'A'; // kputc('A', &str);
        else if (x == 0x2) c[i] = 'C'; //kputc('C', &str);
        else if (x == 0x3) c[i] = 'G'; //kputc('G', &str);
        else if (x == 0x4) c[i] = 'T'; //kputc('T', &str);
        else if (x == 0x5) c[i] = 'N';
        else break;
        i--;
        q = q>>3;
    }
    kstring_t str = {0,0,0};
    kputs(c+i+1, &str);
    return str.s;
}
static int check_Ns(char *s, int l)
{
    if (l == 0) l = strlen(s);
    int i;
    for (i = 0; i < l; ++i)
        if (s[i] != 'A' && s[i] != 'a' && s[i] != 'C' && s[i] != 'c'
            && s[i] != 'G' && s[i] != 'g' && s[i] != 'T' && s[i] != 't') return 1;

    return 0;
}

void ss_print(ss_t *S)
{
    printf("Printing struct SS..\n");
    khint_t k;
    for (k = 0; k != kh_end(S->d0); ++k) {
        if (kh_exist(S->d0, k)) {
            int key = kh_key(S->d0, k);
            char *s = decode64(key);
            printf("%s\n", s);
            free(s);
        }       
    }
    for (k = 0; k != kh_end(S->d1); ++k) {
        if (kh_exist(S->d1, k)) {
            int key = kh_key(S->d1, k);
            char *s = decode64(key);
            printf("%s\n", s);
            free(s);
            sidx_t *idx = &kh_val(S->d1, k);
            int i;
            for (i = 0; i < idx->n; ++i)
                printf("%d ", idx->idx[i]);
            printf("\n");
        }
    }
}
ss_t *ss_init()
{
    ss_t *s = malloc(sizeof(*s));
    memset(s, 0, sizeof(ss_t));
    s->d0 = kh_init(ss64);
    s->d1 = kh_init(ss32);
    return s;
}

void ss_destroy(ss_t *S)
{
    kh_destroy(ss64, S->d0);
    khint_t k;
    for (k = kh_begin(S->d1); k != kh_end(S->d1); ++k) {
        if (kh_exist(S->d1, k)) {
            sidx_t *idx = &kh_val(S->d1, k);
            if (idx &&idx->idx) free(idx->idx);
        }
    }
    kh_destroy(ss32, S->d1);
    free(S->cs);
    free(S);
}

static void build_kmers(ss_t *S, uint64_t q, int idx)
{
    int offset = 3 *kmer_size;
    uint32_t mask = ~(0x1<<offset);
    mask = mask<<(32-offset)>>(32-offset);
    for (;;) {
        if (q>>(offset-3) == 0) break;
        uint32_t x = q & mask;
        q=q>>3;
        khint_t k = kh_get(ss32, S->d1, x);
        if (k != kh_end(S->d1)) {
            struct ss_idx *si = &kh_val(S->d1, k);
            if (si->m == si->n) {
                si->m = si->m<<1;
                si->idx = realloc(si->idx, si->m*sizeof(int));
            }
            si->idx[si->n++] = idx;
        }
        else {
            int ret;
            k = kh_put(ss32, S->d1, x, &ret);
            struct ss_idx *si = &kh_val(S->d1, k);
            memset(si, 0, sizeof(struct ss_idx));
            si->m = 2;
            si->idx = realloc(si->idx, sizeof(int)*si->m);
            si->idx[si->n++] = idx;
        }
    }
}
int ss_push(ss_t *S, char *seq)
{
    int N = check_Ns(seq, 0);
    if (N) error("Try to push sequence %s contain Ns.", seq);
    uint64_t q = enc64(seq);
    khint_t k = kh_get(ss64, S->d0, q);
    if (k != kh_end(S->d0)) return 1;
    if (S->n == S->m) {
        S->m = S->m == 0 ? 1024 : S->m<<1;
        S->cs = realloc(S->cs, S->m*sizeof(uint64_t));
    }
    S->cs[S->n] = q;
    int ret;
    k = kh_put(ss64, S->d0, S->cs[S->n], &ret);
    kh_val(S->d0, k) = S->n;
    build_kmers(S, q, S->n);

    S->n++;
    return 0;
}

struct element {
    int ele;
    int cnt;
};
typedef struct set {
    struct element *ele;
    int n, m;
} set_t;

set_t *set_init()
{
    set_t *set = malloc(sizeof(*set));
    memset(set, 0, sizeof(*set));
    return set;
}
void set_destory(set_t *set)
{
    if (set->m) free(set->ele);
    free(set);
}
static void set_push_core(int ele, set_t *set)
{
    int i;
    for (i = 0; i < set->n; ++i)
        if (set->ele[i].ele == ele) {
            set->ele[i].cnt++;
            return;
        }

    if (set->n == set->m) {
        set->m = set->m == 0 ? 2 : set->m<<1;
        set->ele = realloc(set->ele, sizeof(struct element)*set->m);
    }
    
    set->ele[set->n].ele = ele;
    set->ele[set->n].cnt = 1;
    set->n++;
}
void set_push(int *ele, int n, set_t *set)
{
    assert(n > 0);

    int i;
    for (i = 0; i < n; ++i) set_push_core(ele[i], set);    
}
int cmpfunc (const void * a, const void * b)
{
    return (*(struct element*)b).cnt - (*(struct element*)a).cnt;
}

int set_top_2(set_t *set)
{
    if (set->n <= 2) return set->n;

    qsort(set->ele, set->n,  sizeof(struct element),  cmpfunc);

    int max = set->ele[1].cnt;
    int i;
    for (i = 2; max <= set->ele[i].cnt && i < set->n; ++i) {}
    return i;
}

int hamming_dist_calc(uint64_t a, uint64_t b)
{
    char *s1 = decode64(a);
    char *s2 = decode64(b);
    int l = strlen(s1);
    int i;
    int d = 0;
    for (i = 0; i < l; ++i)
        if (s1[i] != s2[i]) d++;
    free(s1); free(s2);
    return d;
}

extern size_t levenshtein_n(const char *a, const size_t length, const char *b, const size_t bLength);

int levnshn_dist_calc(uint64_t a, uint64_t b)
{
    char *s1 = decode64(a);
    char *s2 = decode64(b);
    int l = strlen(s1);
    int dist = levenshtein_n(s1, l, s2, l);
    free(s1);
    free(s2);
    return dist;
}

// static int use_levenshtein_distance = 0;

// 1 for hamming distance
// 2 for levenshtein distance
// 3 for mixed 
static int dist_strategy = 1;

void set_method(int i)
{
    dist_strategy = i;
}
void set_hamming()
{
    set_method(1);
    //use_levenshtein_distance = 0;
}
void set_levenshtein()
{
    set_method(2);
    //use_levenshtein_distance = 1;    
}
void set_mix()
{
    set_method(3);
}
char *ss_query(ss_t *S, char *seq, int e, int *exact)
{
    *exact = 1; // exactly match
    int l = strlen(seq);
    if (l > kmer_max) error("Sequence is too long. %s", seq);
    uint64_t q = enc64(seq);
    khint_t k = kh_get(ss64, S->d0, q);
    
    if (k != kh_end(S->d0)) return decode64(q); // exactly match

    *exact = 0;
    int i;
    set_t *set = set_init();
    
    for (i = 0; i < l - kmer_size+1; ++i) {
        int j = check_Ns(seq+i, kmer_size);
        if (j) {
            i+=j;
            continue;
        }
        uint32_t q0 = enc32(seq+i, kmer_size);
         k = kh_get(ss32, S->d1, q0);        
        if (k == kh_end(S->d1)) continue;
        
        struct ss_idx *idx = &kh_val(S->d1, k);        
        set_push(idx->idx, idx->n, set);        
    }


    /*
      Previous we use kmer frequency to identify the similar of two sequence and calculate the lenvenstain distance
      for top 2 candidates. However, in some cases, when a sequence error happens at the middle of query sequence, leading
      to low frequency of kmers, will be ignored at this step. So we now will count all candidates. 

      // int n = set_top_2(set);      

      */

    
    int hit = -1;
    for (i = 0; i < set->n; ++i) {
        int dist = 0;
        if (dist_strategy == 1) {
            dist = hamming_dist_calc(S->cs[set->ele[i].ele], q);
        } else if (dist_strategy == 2) {
            dist = levnshn_dist_calc(S->cs[set->ele[i].ele], q);
        } else if (dist_strategy == 3) {
            dist = hamming_dist_calc(S->cs[set->ele[i].ele], q);
        } else {
            error("Unknown dist strategy.");
        }

        if (dist <= e) {
            if (hit != -1) goto multi_hits;
            hit = set->ele[i].ele;
        }
    }
    // mix strategy, if hamming dist not work, use levenshtein instead. Wang Zhifeng report there are ~5% reads offset
    // 1 position in ad153 library, 20220223
    if (hit == -1 && dist_strategy == 2) {
        for (i = 0; i < set->n; ++i) {
            int dist;
            dist = levnshn_dist_calc(S->cs[set->ele[i].ele], q);
            if (dist <= e) {
                if (hit != -1) goto multi_hits;
                hit = set->ele[i].ele;
            }
        }
    }
    
    set_destory(set);

    if (hit == -1) return NULL;
    return decode64(S->cs[hit]);

  multi_hits:
    set_destory(set);
    return NULL;
}

#ifdef SS_MAIN
int main()
{
    ss_t *S = ss_init();    
    ss_push(S,"CTTCGATGGT");
    ss_push(S,"ACTTCTATGC");
    int exact;
    char *s1 = ss_query(S, "CTTCTATGGT", 1, &exact);
    char *s2 = ss_query(S, "ACTTCTATGA", 2, &exact);
    fprintf(stderr, "%s\t%s\n", s1, s2);
    return 0;
}

#endif
