/*
 File:   main.c
 SPDX-License-Identifier: GPL-3.0-or-later

 riakctl: command-line client for Riak KV built on libriak-c.
 Global options and environment variables, command dispatch and help;
 main() itself is in entry.c, so that the tests can link everything else.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "riakctl.h"

#define DEFAULT_URL "pb://127.0.0.1:8087"

static const rc_cmd commands[] = {
    {"ping", rc_cmd_ping, "[--all]",
     "Pings every configured node; fails with exit code 7 when none answers.",
     "  --all            fail unless every node answers\n"},
    {"info", rc_cmd_info, "", "Node name and version, client node health and cluster members.",
     NULL},
    {"stats", rc_cmd_stats, "[NAME|PREFIX* ...]",
     "Node statistics (/stats), optionally only the named entries.\n"
     "Over Protocol Buffers the HTTP port (--http-port) is used.",
     NULL},
    {"get", rc_cmd_get, "[options] BUCKET KEY",
     "Fetches an object. The value is written to stdout as is, unless --json\n"
     "or --all is given.",
     "  -t, --type TYPE       bucket type\n"
     "  -j, --json            the object as JSON (value, content type, vclock,\n"
     "                        metadata, indexes, links)\n"
     "  -a, --all             every sibling, as JSON\n"
     "      --head            metadata only, as JSON\n"
     "      --r Q, --pr Q     read quorums (one, quorum, all, default or a number)\n"
     "      --basic-quorum    --notfound-ok\n"},
    {"put", rc_cmd_put, "[options] BUCKET [KEY]",
     "Stores an object; the value is read from stdin unless --value or --file\n"
     "is given. Without KEY a key is generated and printed as {\"key\": ...}.",
     "  -t, --type TYPE         bucket type\n"
     "  -c, --content-type CT   content type (default application/octet-stream)\n"
     "      --value TEXT        value given on the command line\n"
     "  -f, --file PATH         value read from a file\n"
     "  -m, --meta NAME=VALUE   user metadata (repeatable)\n"
     "  -i, --index NAME=VALUE  secondary index entry, e.g. email_bin=a@b (repeatable)\n"
     "      --link VALUE        raw Link header value (repeatable)\n"
     "      --vclock VCLOCK     vector clock of the version being replaced\n"
     "      --create            fails with exit code 5 when the key exists\n"
     "      --return-body       prints the stored object as JSON\n"
     "      --w Q, --dw Q, --pw Q  write quorums\n"},
    {"delete", rc_cmd_delete, "[options] BUCKET KEY", "Deletes a key.",
     "  -t, --type TYPE       bucket type\n"
     "      --vclock VCLOCK   vector clock of the version being deleted\n"
     "      --rw Q, --w Q, --pw Q, --dw Q  quorums\n"},
    {"list", rc_cmd_list, "buckets | keys BUCKET",
     "Lists bucket names or the keys of a bucket as JSON Lines (one string per\n"
     "line). Both walk every key in the cluster and are expensive.",
     "  -t, --type TYPE   bucket type\n"
     "  -p, --plain       plain lines instead of JSON strings\n"},
    {"backup", rc_cmd_backup, "[options] BUCKET",
     "Writes every object of a bucket (siblings, vector clocks, content types,\n"
     "metadata, indexes and links) or every data type value of a data type\n"
     "bucket as JSON Lines.",
     "  -t, --type TYPE       bucket type\n"
     "  -o, --output PATH     output file (default stdout)\n"
     "      --method METHOD   auto (default), fold, aae or keys:\n"
     "                          fold  object fold (Protocol Buffers, 2i-capable backend)\n"
     "                          aae   AAE key/clock fold (Riak 2.9+, tictac AAE)\n"
     "                          keys  key listing\n"},
    {"restore", rc_cmd_restore, "[options] [FILE]",
     "Restores a backup (default stdin). Running it again changes nothing: keys\n"
     "whose siblings are all present already are skipped.",
     "  -t, --type TYPE     restore into this bucket type instead of the backup's\n"
     "  -b, --bucket NAME   restore into this bucket instead of the backup's\n"
     "  -n, --dry-run       only report what would be written\n"
     "  -j, --jobs N        worker threads, each with its own connection\n"
     "                      (default 4, at most 64)\n"},
    {"index", rc_cmd_index, "[options] BUCKET INDEX VALUE | BUCKET INDEX MIN MAX",
     "Secondary index query (exact match or range). Matches are streamed as JSON\n"
     "Lines: a key string, or {\"key\", \"term\"} with --terms. With --max one\n"
     "page is printed as {\"results\": [...], \"continuation\": ...}.",
     "  -t, --type TYPE        bucket type\n"
     "      --max N            page size\n"
     "      --continuation C   continuation of the previous page\n"
     "      --terms            range queries: the matching terms too\n"
     "      --regex RE         range queries: terms filtered by RE\n"
     "      --sort             sorted results without --max\n"
     "      --timeout MS       server-side timeout\n"
     "      --cover CONTEXT    one coverage plan entry (see coverage)\n"},
    {"query", rc_cmd_query, "[options] BUCKET INDEX | --request FILE BUCKET",
     "Query API request (Riak 3.4+). One index query is given with flags, any\n"
     "request with a JSON document: {\"parts\": [{\"index\", \"start\", \"end\",\n"
     "\"eval\", \"filter\", \"regex\", \"tag\"}], \"aggregation\", \"accumulate\",\n"
     "\"term\", \"substitutions\": {...}, \"max_results\", \"continuation\",\n"
     "\"timeout\"}. The response is printed as returned; queue accumulations\n"
     "(queue_raw_keys, queue_raw_terms) are fetched to the end as JSON Lines.",
     "  -t, --type TYPE        bucket type\n"
     "      --request FILE     request document (- for stdin)\n"
     "      --start T, --end T term range of the index query\n"
     "      --eval EXPR        evaluation expression\n"
     "      --filter EXPR      filter expression\n"
     "      --regex RE         regular expression on the terms\n"
     "      --accumulate OPT   keys (default), raw_keys, terms, count, raw_count, ...\n"
     "      --term NAME        attribute accumulated by the term options\n"
     "      --sub NAME=VALUE   substitution for :NAME (repeatable)\n"
     "      --max N            page or batch size\n"
     "      --continuation C   continuation of the previous page\n"
     "      --timeout S        server-side timeout in seconds\n"},
    {"mapred", rc_cmd_mapred, "[--collect] [FILE]",
     "Runs a MapReduce job (JSON, from FILE or stdin). Phase results are streamed\n"
     "as JSON Lines {\"phase\": N, \"data\": ...}; --collect prints the whole result.",
     NULL},
    {"coverage", rc_cmd_coverage, "[-t TYPE] [--min N] BUCKET",
     "Coverage plan of a bucket (Protocol Buffers), for parallel index queries.", NULL},
    {"preflist", rc_cmd_preflist, "[-t TYPE] BUCKET KEY",
     "Preference list (vnodes holding the replicas) of a key (Protocol Buffers).", NULL},
    {"dt", rc_cmd_dt, "-t TYPE ACTION ...",
     "Data types (CRDTs) in a bucket type with a datatype property.\n"
     "  dt get BUCKET KEY               value and context as JSON\n"
     "  dt incr|decr BUCKET KEY [N]     counter\n"
     "  dt add|remove BUCKET KEY ELEM...  set, grow-only set, HyperLogLog\n"
     "  dt update BUCKET [KEY|-] [FILE] operation document (stdin by default);\n"
     "                                  without KEY a key is generated\n"
     "Operation documents: a number for counters, {\"add\": [...], \"remove\":\n"
     "[...]} for sets, {\"<field>_<type>\": operation, \"remove\": [\"<field>_<type>\"]}\n"
     "for maps (counter: number, register: string, flag: boolean, set and map:\n"
     "nested operation).",
     "  -t, --type TYPE      bucket type (required)\n"
     "      --context CTX    causal context for removals (fetched when absent)\n"
     "      --return-body    prints the new value\n"},
    {"counter", rc_cmd_counter, "get|incr|decr BUCKET KEY [N]",
     "Riak 1.4 counters (allow_mult buckets of the default type).", NULL},
    {"props", rc_cmd_props, "[-t TYPE] ACTION NAME [JSON|-]",
     "Bucket and bucket type properties, as {\"props\": {...}} documents.\n"
     "  props get BUCKET | props set BUCKET [JSON|-] | props reset BUCKET\n"
     "  props type-get TYPE | props type-set TYPE [JSON|-]\n"
     "Bucket types themselves are created and activated with riak admin.",
     NULL},
    {"search", rc_cmd_search, "ACTION ...",
     "Riak Search (Solr): queries, indexes and schemas (Riak 2.x only).\n"
     "  search query INDEX QUERY        matching documents as JSON\n"
     "  search index-list | index-get NAME | index-create NAME | index-delete NAME\n"
     "  search schema-get NAME | schema-put NAME [FILE]  (XML, stdin by default)",
     "      --filter FQ, --sort S, --fields F, --df FIELD, --op AND|OR\n"
     "      --start N, --rows N   paging\n"
     "      --raw                 the Solr response as returned\n"
     "      --schema NAME         index-create: schema (default _yz_default)\n"
     "      --n-val N             index-create: replica count\n"},
    {"aae", rc_cmd_aae, "[options] ACTION ...",
     "AAE folds (Riak 2.9+, tictac AAE). On the cached n_val trees:\n"
     "  aae root NVAL | aae branches NVAL ID... | aae clocks NVAL SEGMENT...\n"
     "  aae buckets NVAL                 buckets holding objects of that n_val\n"
     "On a bucket range (the filter options below):\n"
     "  aae tree BUCKET [--size S]       range tree\n"
     "  aae keys BUCKET                  keys and vector clocks (JSON Lines)\n"
     "  aae find BUCKET --siblings N | --object-size BYTES\n"
     "  aae stats BUCKET                 object count, size and sibling histograms\n"
     "  aae tombs BUCKET                 tombstones\n"
     "  aae reap BUCKET | aae erase BUCKET  count only, unless --local or --job ID\n"
     "  aae repair BUCKET                read repair of the range\n"
     "  aae repl BUCKET QUEUE            queues the range for nextgenrepl\n"
     "  aae compare BUCKET --peer URL | --peer-bucket NAME\n"
     "                                   keys whose clocks differ (JSON Lines);\n"
     "                                   trees for one bucket on two clusters,\n"
     "                                   key by key for two buckets",
     "  -t, --type TYPE           bucket type\n"
     "      --start-key K, --end-key K  key range\n"
     "      --segments A,B,...    segment filter, ids of --tree-size\n"
     "      --tree-size S         xxsmall, xsmall, small, medium, large, xlarge\n"
     "      --modified-start T, --modified-end T  last-modified range (Unix seconds)\n"
     "      --hash-iv N           tree: clocks rehashed with this IV\n"
     "      --size S              tree and compare: tree size (default small)\n"
     "      --local | --job ID    reap and erase: actually change data\n"
     "      --peer URL            compare: the other cluster (same credentials)\n"
     "      --peer-bucket NAME    compare: the other bucket\n"},
    {"repl", rc_cmd_repl, "ACTION ...",
     "Replication. nextgenrepl queues (Riak 2.9+):\n"
     "  repl fetch QUEUE [--max N]   takes changes off the queue (JSON Lines)\n"
     "  repl push QUEUE              queues keys read from stdin as JSON Lines\n"
     "                               {\"type\", \"bucket\", \"key\", \"vclock\"}\n"
     "                               (the output of aae keys)\n"
     "  repl members                 cluster members (Riak 3.0+)\n"
     "riak_repl: repl cluster-id | repl get CLUSTER_ID BUCKET KEY",
     NULL},
};

const rc_cmd *rc_find_cmd(const char *name) {
    for (size_t i = 0; i < sizeof commands / sizeof *commands; i++) {
        if (strcmp(commands[i].name, name) == 0) {
            return &commands[i];
        }
    }
    return NULL;
}

static void usage(FILE *f) {
    fprintf(f, "usage: riakctl [global options] COMMAND [options] [arguments]\n"
               "\n"
               "Global options (environment variable in brackets):\n"
               "  -u, --url URLS          nodes: pb://, pbs://, http:// or https:// host:port,\n"
               "                          comma separated [RIAK_URL] (default " DEFAULT_URL ")\n"
               "      --http-port PORT    HTTP port for stats over Protocol Buffers\n"
               "                          [RIAK_HTTP_PORT] (default 8098 for pb://)\n"
               "      --user NAME         Riak security user [RIAK_USER]\n"
               "      --password-file F   file holding the password [RIAK_PASSWORD holds\n"
               "                          the password itself]\n"
               "      --ca FILE           CA bundle for TLS verification [RIAK_CA]\n"
               "      --cert FILE         client certificate [RIAK_CERT]\n"
               "      --key FILE          client certificate key [RIAK_KEY]\n"
               "      --insecure          the server certificate is not verified\n"
               "      --tls-legacy        interoperability with Riak 2.x TLS [RIAK_TLS_LEGACY=1]\n"
               "      --timeout MS        whole-request timeout (default none)\n"
               "      --connect-timeout MS  connection timeout (default 5000)\n"
               "      --retries N         additional attempts after a failure (default 2)\n"
               "  -t, --type TYPE         bucket type for the commands that take one\n"
               "      --pretty, --compact JSON layout (default: pretty on a terminal)\n"
               "  -v, --verbose           client log on stderr\n"
               "  -V, --version           version\n"
               "  -h, --help              this help\n"
               "\n"
               "Commands:\n");
    for (size_t i = 0; i < sizeof commands / sizeof *commands; i++) {
        fprintf(f, "  %-9s %s\n", commands[i].name, commands[i].usage);
    }
    fprintf(f, "\n'riakctl help COMMAND' describes a command. Exit codes: 0 success, 1 error,\n"
               "2 usage, 3 not found, 4 siblings, 5 precondition failed, 6 unauthorized,\n"
               "7 node unreachable, 8 not supported by the node or transport.\n");
}

static bool env_true(const char *name) {
    const char *v = getenv(name);
    return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0);
}

static const char *env(const char *name) {
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

enum {
    OPT_HTTP_PORT = 256,
    OPT_USER,
    OPT_PASSWORD_FILE,
    OPT_CA,
    OPT_CERT,
    OPT_KEY,
    OPT_INSECURE,
    OPT_TLS_LEGACY,
    OPT_TIMEOUT,
    OPT_CONNECT_TIMEOUT,
    OPT_RETRIES,
    OPT_PRETTY,
    OPT_COMPACT
};

static int run(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {
        {"url", required_argument, NULL, 'u'},
        {"http-port", required_argument, NULL, OPT_HTTP_PORT},
        {"user", required_argument, NULL, OPT_USER},
        {"password-file", required_argument, NULL, OPT_PASSWORD_FILE},
        {"ca", required_argument, NULL, OPT_CA},
        {"cert", required_argument, NULL, OPT_CERT},
        {"key", required_argument, NULL, OPT_KEY},
        {"insecure", no_argument, NULL, OPT_INSECURE},
        {"tls-legacy", no_argument, NULL, OPT_TLS_LEGACY},
        {"timeout", required_argument, NULL, OPT_TIMEOUT},
        {"connect-timeout", required_argument, NULL, OPT_CONNECT_TIMEOUT},
        {"retries", required_argument, NULL, OPT_RETRIES},
        {"type", required_argument, NULL, 't'},
        {"pretty", no_argument, NULL, OPT_PRETTY},
        {"compact", no_argument, NULL, OPT_COMPACT},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};
    rc_conn *c = &x->conn;
    riak_config *cfg = &c->cfg;
    riak_config_init(cfg);
    cfg->connect_timeout_ms = 5000;
    const char *url = env("RIAK_URL");
    const char *http_port = env("RIAK_HTTP_PORT");
    const char *password_file = NULL;
    cfg->username = env("RIAK_USER");
    cfg->ca_file = env("RIAK_CA");
    cfg->tls_cert_file = env("RIAK_CERT");
    cfg->tls_key_file = env("RIAK_KEY");
    cfg->tls_legacy = env_true("RIAK_TLS_LEGACY");
    x->pretty = isatty(STDOUT_FILENO);
    unsigned long long n;
    int ch;
    rc_getopt_reset();
    while ((ch = getopt_long(argc, argv, "+u:t:vVh", opts, NULL)) != -1) {
        switch (ch) {
        case 'u':
            url = optarg;
            break;
        case OPT_HTTP_PORT:
            http_port = optarg;
            break;
        case OPT_USER:
            cfg->username = optarg;
            break;
        case OPT_PASSWORD_FILE:
            password_file = optarg;
            break;
        case OPT_CA:
            cfg->ca_file = optarg;
            break;
        case OPT_CERT:
            cfg->tls_cert_file = optarg;
            break;
        case OPT_KEY:
            cfg->tls_key_file = optarg;
            break;
        case OPT_INSECURE:
            cfg->tls_verify = false;
            break;
        case OPT_TLS_LEGACY:
            cfg->tls_legacy = true;
            break;
        case OPT_TIMEOUT:
        case OPT_CONNECT_TIMEOUT:
            if (!rc_parse_uint(optarg, 86400000, &n)) {
                fprintf(stderr, "riakctl: invalid timeout '%s'\n", optarg);
                return RC_USAGE;
            }
            *(ch == OPT_TIMEOUT ? &cfg->timeout_ms : &cfg->connect_timeout_ms) = (long)n;
            break;
        case OPT_RETRIES:
            if (!rc_parse_uint(optarg, 100, &n)) {
                fprintf(stderr, "riakctl: invalid retry count '%s'\n", optarg);
                return RC_USAGE;
            }
            cfg->max_retries = (unsigned)n;
            break;
        case 't':
            x->type = optarg;
            break;
        case OPT_PRETTY:
        case OPT_COMPACT:
            x->pretty = ch == OPT_PRETTY;
            break;
        case 'v':
            c->verbose = true;
            break;
        case 'V':
            printf("riakctl (libriak-c) %d.%d.%d\n", RIAK_VERSION_MAJOR, RIAK_VERSION_MINOR,
                   RIAK_VERSION_PATCH);
            return RC_OK;
        case 'h':
            usage(stdout);
            return RC_OK;
        default:
            usage(stderr);
            return RC_USAGE;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return RC_USAGE;
    }
    const char *name = argv[optind];
    if (strcmp(name, "help") == 0) {
        if (optind + 1 >= argc) {
            usage(stdout);
            return RC_OK;
        }
        const rc_cmd *cmd = rc_find_cmd(argv[optind + 1]);
        if (cmd == NULL) {
            fprintf(stderr, "riakctl: unknown command '%s'\n", argv[optind + 1]);
            return RC_USAGE;
        }
        return rc_cmd_help(cmd);
    }
    const rc_cmd *cmd = rc_find_cmd(name);
    if (cmd == NULL) {
        fprintf(stderr, "riakctl: unknown command '%s'; 'riakctl help' lists them\n", name);
        return RC_USAGE;
    }

    int rc = rc_conn_setup(c, url ? url : DEFAULT_URL);
    if (rc != RC_OK) {
        return rc;
    }
    if (http_port) {
        if (!rc_parse_uint(http_port, 65535, &n)) {
            fprintf(stderr, "riakctl: invalid HTTP port '%s'\n", http_port);
            return RC_USAGE;
        }
        cfg->http_port = (unsigned)n;
    } else if (cfg->transport == RIAK_TRANSPORT_PB && !cfg->tls) {
        cfg->http_port = 8098;
    }
    for (size_t i = 0; i < c->n_nodes; i++) {
        c->nodes[i].http_port = cfg->http_port;
    }
    if (password_file) {
        if ((c->password = rc_read_password_file(password_file)) == NULL) {
            return RC_ERROR;
        }
    } else if (env("RIAK_PASSWORD")) {
        if ((c->password = strdup(env("RIAK_PASSWORD"))) == NULL) {
            return RC_ERROR;
        }
    }
    cfg->password = c->password;
    if (cfg->username && cfg->password == NULL) {
        fprintf(stderr, "riakctl: --user needs a password (RIAK_PASSWORD or --password-file)\n");
        return RC_USAGE;
    }
    if (cfg->tls_cert_file && cfg->tls_key_file == NULL) {
        cfg->tls_key_file = cfg->tls_cert_file;
    }

    int sub_argc = argc - optind;
    char **sub_argv = argv + optind;
    for (int i = 1; i < sub_argc; i++) {
        if (strcmp(sub_argv[i], "--") == 0) {
            break;
        }
        if (strcmp(sub_argv[i], "-h") == 0 || strcmp(sub_argv[i], "--help") == 0) {
            return rc_cmd_help(cmd);
        }
    }
    rc_getopt_reset();
    return cmd->fn(x, sub_argc, sub_argv);
}

int rc_main(int argc, char **argv) {
    if (riak_global_init() != RIAK_OK) {
        fprintf(stderr, "riakctl: cannot initialize libcurl\n");
        return RC_ERROR;
    }
    rc_ctx x;
    memset(&x, 0, sizeof x);
    int rc = run(&x, argc, argv);
    riak_client_free(x.client);
    rc_conn_free(&x.conn);
    riak_global_cleanup();
    if (fflush(stdout) != 0 && rc == RC_OK) {
        rc = RC_ERROR;
    }
    return rc;
}
