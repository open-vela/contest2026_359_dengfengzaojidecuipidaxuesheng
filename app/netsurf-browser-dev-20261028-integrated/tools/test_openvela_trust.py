#!/usr/bin/env python3
"""Exercise production trust-file filtering with a controlled parser callback."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
source=(root/'platform/openvela/openvela_desktop.c').read_text()
body=source[source.index('static void load_trust('):source.index('static void *run(')]
pre='''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
static const char *test_path;
#define OV_NETSURF_CA_PATH test_path
struct desktop {struct {const unsigned char *ca_pem;size_t ca_size;} trust;};
static int glass_https_validate_ca(const unsigned char *pem,size_t size) {
 assert(size && pem[size-1]==0 && strlen((const char *)pem)+1==size);
 return strstr((const char *)pem,"UNSUPPORTED") ? -1 : 0;
}
'''
post='''
static const char *begin="-----BEGIN CERTIFICATE-----\\n";
static const char *end="-----END CERTIFICATE-----";
static void check(const char *middle, bool present, unsigned count) {
 FILE *f=fopen(test_path,"wb");assert(f);fputs(middle,f);assert(!fclose(f));
 struct desktop d={0};load_trust(&d);assert((d.trust.ca_pem!=NULL)==present);
 if(present) {
  const char *s=(const char *)d.trust.ca_pem;unsigned n=0;
  while((s=strstr(s,begin))){n++;s++;}assert(n==count);
  assert(!strstr((const char *)d.trust.ca_pem,"UNSUPPORTED"));
  assert(d.trust.ca_size==strlen((const char *)d.trust.ca_pem)+1);
  free((void *)d.trust.ca_pem);
 }
}
int main(int argc,char **argv) {
 assert(argc==2);test_path=argv[1];char good[256],bad[256],mixed[2048];
 snprintf(good,sizeof(good),"%sGOOD\\n%s\\n",begin,end);
 snprintf(bad,sizeof(bad),"%sUNSUPPORTED\\n%s\\n",begin,end);
 snprintf(mixed,sizeof(mixed),"%s%s%s",good,bad,good);check(mixed,true,2);
 check(bad,false,0);check("",false,0);check("not a PEM",false,0);
 snprintf(mixed,sizeof(mixed),"%s%sbroken",good,begin);check(mixed,false,0);
 good[strlen(good)-1]=0;check(good,true,1); /* last newline optional */
 snprintf(mixed,sizeof(mixed),"%s%s",good,good);check(mixed,false,0);
 puts("PASS: mixed supported roots, unsupported-only, truncated/empty input, missing final newline and malformed boundaries");
}
'''
(out/'trust.c').write_text(pre+body+post)
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
               '-g',str(out/'trust.c'),'-o',str(out/'trust')],check=True)
subprocess.run([str(out/'trust'),str(out/'roots.pem')],check=True)
