/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_GETOPT_H
#define HAX_WIN32_GETOPT_H

#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt_long(int argc, char *const argv[], const char *short_options,
                const struct option *long_options, int *long_index);

#endif /* HAX_WIN32_GETOPT_H */
