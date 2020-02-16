#include "utils.h"
#include "htslib/kseq.h"
#include "fastq.h"
#include "dict.h"
#include "read_tags.h"
#include "htslib/kstring.h"

static struct args {
    const char           *read_fname;
    struct fastq_handler *fastq;
    struct dict          *tag_dict;
    int                   n_tag;
    int                   min;
} args = {
    .read_fname  = NULL,
    .fastq       = NULL,
    .tag_dict    = NULL,
    .n_tag       = 0,
    .min         = 100,
};

static int parse_args(int argc, char **argv)
{
    if ( argc == 1 ) return 1;

    const char *tags = 0;
    const char *min  = 0;
    int i;
    for (i = 1; i < argc;) {
        const char *a = argv[i++];
        const char **var = 0;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return 1;
        else if (strcmp(a, "-tags") == 0) var = &tags;
        else if (strcmp(a, "-min") == 0) var = &min;

        if (var != 0) {
            if (i == argc) error("Miss an argument after %s.", a);
            *var = argv[i++];
            continue;
        }
        if (a[0] == '-' && a[1]) error("Unknown parameter %s.", a);
        if (args.read_fname == 0) {
            args.read_fname = a;
            continue;
        }

        error("Unknown argument: %s, use -h see help information.", a);
    }

    if (args.read_fname == NULL && (!isatty(fileno(stdin)))) args.read_fname = "-";
    if (args.read_fname == NULL) error("Fastq file(s) must be set.");

    if (tags == NULL) error("-tags is required.");
    
    args.fastq = fastq_handler_init(args.read_fname, NULL, 0, 1000000);
    if (args.fastq == NULL) error("Failed to read fastq.");

    args.tag_dict = str2tag(tags);
    args.n_tag    = dict_size(args.tag_dict);
    if (min) args.min = atoi(min);
    if (args.min < 1) args.min = 1;
    return 0;
}

static int usage()
{
    fprintf(stderr, "LFR_trans_cov -tags CB,UB,GN [-min 100] in.fq\n");
    return 1;
}
int main(int argc, char **argv)
{
    if (parse_args(argc, argv)) return usage();

    struct dict *bc = dict_init();
    kstring_t str={0,0,0};
    int alloc = 0;
    int *counts = NULL;
    
    for (;;) {
        struct bseq_pool *b = fastq_read(args.fastq, &args);
        if (b == NULL) break;
        int i;
        for (i = 0; i < b->n; ++i) {
            struct bseq *s = &b->s[i];
            char **v = fastq_name_pick_tags(s->n0, args.tag_dict);
            str.l= 0;
            int j;
            for (j = 0; j < args.n_tag; ++j)
                if (v[j]) kputs(v[j], &str);
            int idx;
            idx = dict_query(bc, str.s);
            if (idx == -1) idx = dict_push(bc, str.s);
            if (idx >= alloc) {
                counts = realloc(counts, (idx+100)*sizeof(int));
                for (; alloc < idx+100; ++alloc) counts[alloc] = 0;
            }
            counts[idx] += s->l0;
        }
    }

    int max = 0;
    int i;
    for (i = 100; i < alloc; ++i)
        if (counts[i] > max ) max = counts[i];

    int *stats = malloc(sizeof(int)*(max+1));
    memset(stats, 0, sizeof(int)*(max+1));
    for (i = 0; i < alloc; ++i) stats[counts[i]]++;

    for (i = 0; i < max+1; ++i)
        if (stats[i] >0) printf("%d\t%d\n", i, stats[i]);
    
    if (str.m) free(str.s);
    if (alloc) free(counts);
    free(stats);
    dict_destroy(bc);
    dict_destroy(args.tag_dict);
    fastq_handler_destory(args.fastq);

    return 0;
}
