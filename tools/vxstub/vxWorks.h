/* make vxcheck 用の仮のヘッダ(VxWorks の本物ではない。文法を確かめるためだけ)。 */
#ifndef VXSTUB_VXWORKS_H
#define VXSTUB_VXWORKS_H
#include <stdint.h>
#include <stddef.h>
typedef int STATUS;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#endif
