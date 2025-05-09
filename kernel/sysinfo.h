/*
 * @Author: lxk liuxikun4896674@163.com
 * @Date: 2025-04-15 17:06:28
 * @LastEditors: lxk liuxikun4896674@163.com
 * @LastEditTime: 2025-05-09 10:42:48
 * @FilePath: /xv6-labs-2020/kernel/sysinfo.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */

struct sysinfo {
  uint64 freemem;   // amount of free memory (bytes)空闲内存的字节数
  uint64 nproc;     // number of process当前的进程数
};
