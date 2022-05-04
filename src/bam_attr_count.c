// Count all reads and reads with predefined tag for each cell barcode in the bam file
#include "utils.h"
#include "number.h"
#include "htslib/khash.h"
#include "htslib/kstring.h"
#include "htslib/sam.h"
#include "htslib/kseq.h"
#include "dict.h"
#include <zlib.h>
#include "biostring.h"
#include "read_anno.h"

static struct args {
    const char *input_fname;
    const char *output_fname;
    const char *barcode_fname;
    const char *cb_tag; // attribute in BAM
    const char *group_tag;
    int n_tag;
    char **tags;
    int dedup;
    int qual_thres;
    int ignore_header;
    int is_dyn_alloc;
    int file_th;
    int all_tags;

    const char *region_type_tag;
    int n_type;
    enum exon_type *region_types;

} args = {
    .input_fname   = NULL,
    .output_fname  = NULL,
    .barcode_fname = NULL,
    .cb_tag        = NULL,
    .group_tag     = NULL,
    .n_tag         = 0,
    .tags          = NULL,
    .dedup         = 0,
    .qual_thres    = 0,
    .ignore_header = 0,
    .is_dyn_alloc  = 1,
    .file_th       = 4,
    .all_tags      = 0,
    .region_type_tag = "RE",
    .n_type        = 0,
    .region_types  = NULL,

};

static int parse_args(int argc, char **argv)
{
    int i;
    const char *tag  = NULL;
    const char *qual = NULL;
    const char *file_th = NULL;
    const char *region_types = NULL;
    for (i = 1; i < argc; ) {
        const char *a = argv[i++];
        const char **var = 0;
        if (strcmp(a, "-o") == 0 ) var = &args.output_fname;
        else if (strcmp(a, "-cb") == 0) var = &args.cb_tag;
        else if (strcmp(a, "-tags") == 0) var = &tag;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return 1;
        else if (strcmp(a, "-dedup") == 0) {
            args.dedup = 1;
            continue;
        }
        else if (strcmp(a, "-group") == 0) var = &args.group_tag;
        else if (strcmp(a, "-list") == 0) var = &args.barcode_fname;
        else if (strcmp(a, "-q") == 0) var = &qual;
        else if (strcmp(a, "-@") == 0) var = &file_th;
        else if (strcmp(a, "-all-tags") == 0) {
            args.all_tags = 1;
            continue;
        }
        else if (strcmp(a, "-ttag") == 0) var = &args.region_type_tag;
        else if (strcmp(a, "-ttype") == 0) var = &region_types;

        if (var != 0) {
            if (i == argc) error("Miss an argument after %s.", a);
            *var = argv[i++];
            continue;
        }
        if (args.input_fname == NULL) {
            args.input_fname = a;
            continue;
        }
        error("Unknown argument: %s", a);
    }

    CHECK_EMPTY(args.cb_tag, "-cb must be set.");
    CHECK_EMPTY(args.input_fname, "Input bam must be set.");
    
    if (tag) {
        kstring_t str = {0,0,0};
        kputs(tag, &str);
        int n = 0;
        int *s = ksplit(&str, ',', &n);
        args.tags = malloc(n*sizeof(char*));
        args.n_tag = n;
        int j;
        for (j = 0; j < n; ++j) {
            args.tags[j] = strdup(str.s + s[j]);
            if (strlen(args.tags[j]) != 2) error("Unknown tag format. Only two character allowed.");
        }
        free(s);
        free(str.s);
    }
    
    if (qual) args.qual_thres = str2int((char*)qual);
    if (args.qual_thres) args.qual_thres = 0;
    if (file_th) args.file_th = str2int(file_th);
    assert (args.file_th > 0);

    if (region_types) {
        kstring_t str = {0,0,0};
        int n = 0;
        kputs(region_types, &str);
        int *s = str_split(&str, &n);
        if (n == 0) error("Failed to parse -ttype, %s", region_types);
        args.n_type = n;
        args.region_types = malloc(n*sizeof(enum exon_type));
        int k;
        for (k = 0; k < n; ++k) {
            char *rt = str.s+s[k];
            if (strlen(rt) != 1) error("Failed to parse -ttype, %s", region_types);
            enum exon_type type = RE_type_map(rt[0]);
            if (type == type_unknown) error("Unknown type %s", rt);
            args.region_types[k] = type;
        }
        free(s);
        free(str.s);
    }

    return 0;
}
struct counts_per_bcode {
    int n, m; // init equal n_tag, if set group, m == n_tag*n_group
    struct dict **counts; 
};

struct counts {
    struct dict *bc_dict;
    struct dict *group_dict;
    int n, m;
    struct counts_per_bcode *counts;    // n_barcodes
};
void counts_destroy(struct counts *cnt)
{
    int i;
    for (i = 0; i < dict_size(cnt->bc_dict); ++i) {
        struct counts_per_bcode *c = &cnt->counts[i];
        int j;
        for (j = 0; j < c->n; ++j)
            if (c->counts[j]) dict_destroy(c->counts[j]);
        free(c->counts);
    }
    free(cnt->counts);
    dict_destroy(cnt->bc_dict);
    if (cnt->group_dict) dict_destroy(cnt->group_dict);
    free(cnt);
}
int counts_push(struct counts *cnt, bam1_t *b)
{
    int i;
    if (args.all_tags == 1) { // check if all tags existed
        for (i = 0; i < args.n_tag; ++i) {
            uint8_t *va = bam_aux_get(b, args.tags[i]);
            if (!va) return 0;
        }
    }
    

    uint8_t *tag = bam_aux_get(b, args.cb_tag);
    if (!tag) return 1; // skip records without cell Barcodes

    char *name = (char*)(tag+1);
    int id = -1; // individual index
    
    if (args.is_dyn_alloc == 0) {
        id = dict_query(cnt->bc_dict, name);
        if (id == -1) return 1;
        dict_push(cnt->bc_dict, name); // increase count
    }
    else {
        id = dict_push(cnt->bc_dict, name);
        if (dict_size(cnt->bc_dict) >= cnt->m) {
            cnt->m = cnt->m == 0 ? 1024 : cnt->m<<1;
            cnt->counts = realloc(cnt->counts, cnt->m*sizeof(struct counts_per_bcode));
            // init new allocated records
            int i;
            for (i = cnt->n; i < cnt->m; ++i) {
                struct counts_per_bcode *bc = &cnt->counts[i];
                memset(bc, 0, sizeof(*bc));
            }
            cnt->n = cnt->m;
        }
    }
    struct counts_per_bcode *bc = &cnt->counts[id];

    // check region types
    if (args.n_type > 0) {
        
        uint8_t *data = bam_aux_get(b, args.region_type_tag);
        if (!data) return 1; // no RE tag

        int region_type_flag = 0;
        int k;
        for (k = 0; k < args.n_type; ++k) {
            if (args.region_types[k] == RE_type_map(data[1])) {
                region_type_flag = 1;
                break;
            }
        }
        
        if (region_type_flag == 0) return 1;// RE not matched
    }

    // dynamic allocate group tag
    int grp_id = 0; // group index
    
    if (args.group_tag) {
        uint8_t *tag = bam_aux_get(b, args.group_tag);
        if (!tag) return 1;
        grp_id = dict_push(cnt->group_dict, (char*)(tag+1));
    }
    int alloc_group = grp_id+1;

    if (bc->m < alloc_group*args.n_tag) {
        bc->m = alloc_group*args.n_tag;
        bc->counts = realloc(bc->counts, bc->m*sizeof(void*));
        int i;
        for (i = bc->n; i < bc->m; ++i) bc->counts[i] = NULL;
        bc->n = bc->m;
    }
    for (i = 0; i < args.n_tag; ++i) {
        uint8_t *va = bam_aux_get(b, args.tags[i]);
        if (!va) continue;
        int idx = grp_id*args.n_tag+i;
        struct dict *d = bc->counts[idx];
        if (d == NULL) d = dict_init();
        dict_push(d, (char*)(va+1));
        bc->counts[idx] = d;
    }
    
    return 0;
}

int generat_outputs(struct counts *cnt)
{
    FILE *out = args.output_fname == NULL ? stdout : fopen(args.output_fname, "w");
    if (out == NULL) {
        warnings("%s : %s.", args.output_fname, strerror(errno));
        return 1;
    }

    // header
    if (args.ignore_header == 0) {
        fputs("BARCODE\tRaw", out);
        if (args.group_tag) {
            int i;
            for (i = 0; i < dict_size(cnt->group_dict); ++i) {
                int j;
                for (j = 0; j < args.n_tag; ++j) {
                    fputc('\t', out);
                    fputs(dict_name(cnt->group_dict,i), out);
                    fputc('_', out);
                    fputs(args.tags[j], out);
                }         
            }
        }
        else {
            int i;
            for (i = 0; i < args.n_tag; ++i) {
                fputc('\t', out);
                fputs(args.tags[i], out);
            }
        }
        fputc('\n', out);
    }

    int i;
    int w = args.group_tag == NULL ? args.n_tag : args.n_tag*dict_size(cnt->group_dict);
    
    for (i = 0; i < dict_size(cnt->bc_dict); ++i) {
        struct counts_per_bcode *bcode = &cnt->counts[i];
        fprintf(out, "%s\t%u", dict_name(cnt->bc_dict, i), dict_count(cnt->bc_dict, i) );

        int j;
        for (j = 0; j < w; ++j) {
            if ( j >= bcode->n) fputs("\t0", out);
            else {
                fprintf(out, "\t%u", bcode->counts[j] == NULL ? 0:
                        args.dedup == 1 ? dict_size(bcode->counts[j]) : dict_count_sum(bcode->counts[j]));
            }
        }
        fputc('\n', out);
    }

    fclose(out);
    return 0;
}

extern int bam_attr_usage();

int bam_count_attr(int argc, char *argv[])
{
    double t_real;
    t_real = realtime();

    if (parse_args(argc, argv)) return bam_attr_usage();
    
    htsFile *fp  = hts_open(args.input_fname, "r");
    CHECK_EMPTY(fp, "%s : %s.", args.input_fname, strerror(errno));
    htsFormat type = *hts_get_format(fp);
    if (type.format != bam && type.format != sam)
        error("Unsupported input format, only support BAM/SAM/CRAM format.");
    bam_hdr_t *hdr = sam_hdr_read(fp);
    CHECK_EMPTY(hdr, "Failed to open header.");

    hts_set_threads(fp, args.file_th);
    
    struct counts *cnt = malloc(sizeof(*cnt));
    memset(cnt, 0, sizeof(*cnt));
    cnt->bc_dict = dict_init();
    if (args.group_tag) cnt->group_dict = dict_init();
    
    if (args.barcode_fname) {
        dict_read(cnt->bc_dict, args.barcode_fname);
        cnt->m = dict_size(cnt->bc_dict);
        cnt->n = cnt->m;
        cnt->counts = malloc( cnt->m *sizeof(struct counts_per_bcode));
        int i;
        for (i = 0; i < cnt->n; ++i) {
            struct counts_per_bcode *bcode = &cnt->counts[i];
            memset(bcode, 0, sizeof(struct counts_per_bcode));
        }
        args.is_dyn_alloc = 0;
    }
    
    bam1_t *b;
    int ret;
    b = bam_init1();

    while ((ret = sam_read1(fp, hdr, b)) >= 0) {
        if (b->core.flag & BAM_FSECONDARY) continue; // filter secondary alignments

        /* For raw reads per barcode, unmapped reads also included. */
        //if (b->core.tid < 0) continue;

        // if set qual threshold, only keep confidently mapped reads
        if (b->core.qual < args.qual_thres) continue;
        
        counts_push(cnt, b);
    }

    bam_destroy1(b);
    bam_hdr_destroy(hdr);
    sam_close(fp);

    if (ret != -1) warnings("Truncated file?");

    generat_outputs(cnt);
    
    counts_destroy(cnt);

    LOG_print("Real time: %.3f sec; CPU: %.3f sec", realtime() - t_real, cputime());
    return 0;    
}


        
