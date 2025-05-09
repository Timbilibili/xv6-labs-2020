/*
 * @Author: lxk liuxikun4896674@163.com
 * @Date: 2025-04-15 17:06:28
 * @LastEditors: lxk liuxikun4896674@163.com
 * @LastEditTime: 2025-05-07 16:21:01
 * @FilePath: /xv6-labs-2020/user/trace.c
 */
#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int i;
  char *nargv[MAXARG];

  if(argc < 3 || (argv[1][0] < '0' || argv[1][0] > '9')){
    fprintf(2, "Usage: %s mask command\n", argv[0]);
    exit(1);
  }

  if (trace(atoi(argv[1])) < 0) {
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }
  
  for(i = 2; i < argc && i < MAXARG; i++){
    nargv[i-2] = argv[i];
  }
  exec(nargv[0], nargv);
  exit(0);
}
