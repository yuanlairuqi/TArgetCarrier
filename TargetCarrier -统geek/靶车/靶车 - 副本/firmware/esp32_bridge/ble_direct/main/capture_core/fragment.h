#ifndef CAP_FRAGMENT_H
#define CAP_FRAGMENT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FRAGMENT_SIZE 20
#define FRAGMENT_DATA 12
#define FRAGMENT_MAX_BYTES 72
/* A separate reassembler per source/boot; caller authenticates outer packet. */
typedef struct {uint32_t batch_id,started_ms;uint8_t count,next,used;bool active;uint8_t bytes[FRAGMENT_MAX_BYTES];} frag_join;
bool frag_encode(const uint8_t *,size_t,uint32_t,unsigned,uint8_t *);
/* Returns 1 complete, 0 pending, -1 rejected; monotonic uint32 ms. */
int frag_accept(frag_join *,const uint8_t *,uint32_t,const uint8_t **,size_t *);
#endif
