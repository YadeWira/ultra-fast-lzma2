#include "mem.h"

#ifndef UF2_DATA_BLOCK_H_
#define UF2_DATA_BLOCK_H_

#if defined (__cplusplus)
extern "C" {
#endif

typedef struct {
    const BYTE* data;
    size_t start;
    size_t end;
} UF2_dataBlock;

#if defined (__cplusplus)
}
#endif

#endif /* UF2_DATA_BLOCK_H_ */