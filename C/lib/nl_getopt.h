#ifndef NL_GETOPT_H
#define NL_GETOPT_H

extern int nl_optind;
extern int nl_opterr;
extern char *nl_optarg;

int nl_getopt(int argc, char *const argv[], const char *opts);

#define getopt(ac, av, opts) nl_getopt(ac, av, opts)
#define optarg nl_optarg
#define optind nl_optind

#endif
