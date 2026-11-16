// The checker checked: every built-in node and hub app passes it, and each
// deliberately broken node (tests/support/gr_bad_nodes.c) fails the check
// for the rule it breaks. Part of `make test`.
#include <stdio.h>
#include <string.h>

#include "gr_check_builtin.h"

extern const gr_ck_subject gr_bad_subjects[];
extern const char *const gr_bad_expect[];
extern const int gr_bad_count;

int main(void) {
  gr_ck_options o;
  gr_ck_defaults(&o);
  o.seconds = 1.0;
  o.timeout_s = 120;
  int failures = 0, checks = 0;
  for (int i = 0; i < gr_bad_count; ++i) {
    gr_ck_report r;
    gr_ck_run(&gr_bad_subjects[i], &o, &r);
    ++checks;
    int caught = gr_ck_failed(&r, gr_bad_expect[i]);
    char also[160] = "";
    for (int k = 0; k < r.n; ++k)
      if (r.line[k].ok == 0 && strcmp(r.line[k].part, gr_bad_expect[i]) != 0 && strlen(also) < 120) {
        strcat(also, " ");
        strcat(also, r.line[k].part);
      }
    const char *why = "";
    for (int k = 0; k < r.n; ++k)
      if (r.line[k].ok == 0 && strcmp(r.line[k].part, gr_bad_expect[i]) == 0) why = r.line[k].text;
    printf("%s %-12s must fail %-10s %s%s%s\n", caught ? "caught" : "MISSED", gr_bad_subjects[i].desc->name[0] == 'b' && strlen(gr_bad_subjects[i].desc->name) < 13 ? gr_bad_subjects[i].desc->name : "bad_name", gr_bad_expect[i], why,
           also[0] ? "; also:" : "", also);
    failures += !caught;
  }
  for (int i = 0; i < gr_ck_builtin_count; ++i) {
    gr_ck_report r;
    gr_ck_run(&gr_ck_builtin[i], &o, &r);
    ++checks;
    if (r.fails) {
      printf("FAIL built-in %s:", gr_ck_builtin[i].name);
      for (int k = 0; k < r.n; ++k)
        if (r.line[k].ok == 0) printf(" [%s] %s", r.line[k].part, r.line[k].text);
      printf("\n");
      ++failures;
    }
  }
  printf("test_graph_check: %d checks (%d broken nodes, %d built-ins), %d failures\n", checks, gr_bad_count,
         gr_ck_builtin_count, failures);
  return failures ? 1 : 0;
}
