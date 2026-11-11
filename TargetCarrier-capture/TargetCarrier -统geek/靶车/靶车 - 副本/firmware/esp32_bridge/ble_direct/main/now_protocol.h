#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define NOW_FRAME_SIZE 64
#define NOW_QUEUE_MAX_AGE_MS 100
#define NOW_QUERY 1
#define NOW_COMMAND 2
#define ROBOT_STOP 0
#define NOW_OK 0
#define NOW_BUSY 1
#define NOW_CHALLENGE 2
#define NOW_NEED_STOP 3
#define NOW_SESSION 4
#define NOW_REPLAY 5
#define NOW_BAD_COMMAND 6
#define NOW_STALE 7
typedef struct {uint8_t kind;uint32_t node,boot,epoch,session,seq;uint8_t command[20];} now_request_t;
uint32_t now_crc32(const uint8_t *,size_t);
bool now_decode(const uint8_t *,size_t,now_request_t *);
