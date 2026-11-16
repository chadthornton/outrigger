// graph_check: does a node or an app keep lib/graph's contract
// (lib/graph/CONTRACT.md)? The vibe-check idea, for our C node contract.
//   build/graph_check [--list] [--node NAME] [--seconds S] [--seed N] [--json] [--no-isolate]
// Exit 0 = every check passed (KNWN lines are known third-party failures),
// 1 = a check failed, 2 = could not run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gr_check_builtin.h"

static void json_str(const char *s) {
  putchar('"');
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') putchar('\\');
    putchar(*s);
  }
  putchar('"');
}

int main(int argc, char **argv) {
  gr_ck_options o;
  gr_ck_defaults(&o);
  const char *only = 0;
  int json = 0, list = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--node") && i + 1 < argc) only = argv[++i];
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) o.seconds = atof(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) o.seed = (uint32_t)strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--json")) json = 1;
    else if (!strcmp(argv[i], "--list")) list = 1;
    else if (!strcmp(argv[i], "--no-isolate")) o.isolate = 0;
    else {
      fprintf(stderr, "usage: graph_check [--list] [--node NAME] [--seconds S] [--seed N] [--json] [--no-isolate]\n");
      return 2;
    }
  }
  if (list) {
    for (int i = 0; i < gr_ck_builtin_count; ++i) printf("%s\n", gr_ck_builtin[i].name);
    return 0;
  }
  int ran = 0, failed = 0, passed_checks = 0, failed_checks = 0;
  if (json) printf("[");
  for (int i = 0; i < gr_ck_builtin_count; ++i) {
    const gr_ck_subject *s = &gr_ck_builtin[i];
    if (only && strcmp(only, s->name) != 0) continue;
    gr_ck_report r;
    gr_ck_run(s, &o, &r);
    ran++;
    failed += r.fails > 0;
    if (json) {
      printf("%s{\"name\":", ran > 1 ? "," : "");
      json_str(r.name);
      printf(",\"fails\":%d,\"checks\":[", r.fails);
      for (int k = 0; k < r.n; ++k) {
        printf("%s{\"part\":", k ? "," : "");
        json_str(r.line[k].part);
        printf(",\"ok\":%s,\"text\":", r.line[k].ok == 1 ? "true" : r.line[k].ok == 0 ? "false" : r.line[k].ok == -2 ? "\"known\"" : "null");
        json_str(r.line[k].text);
        printf("}");
      }
      printf("]}");
    } else {
      printf("%s %s\n", r.fails ? "FAIL" : "ok  ", r.name);
      for (int k = 0; k < r.n; ++k) {
        const gr_ck_line *l = &r.line[k];
        printf("  %s %-10s %s\n", l->ok == 1 ? "PASS" : l->ok == 0 ? "FAIL" : l->ok == -2 ? "KNWN" : "----", l->part, l->text);
      }
    }
    for (int k = 0; k < r.n; ++k) passed_checks += r.line[k].ok == 1, failed_checks += r.line[k].ok == 0;
  }
  if (json) printf("]\n");
  if (!ran) {
    fprintf(stderr, "graph_check: no subject named \"%s\" (--list)\n", only ? only : "");
    return 2;
  }
  if (!json)
    printf("graph_check: %d subjects, %d failed; %d checks passed, %d failed; not checked: %s\n", ran, failed,
           passed_checks, failed_checks, gr_ck_not_checked);
  return failed ? 1 : 0;
}
