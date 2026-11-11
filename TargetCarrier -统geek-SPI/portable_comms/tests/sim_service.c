#include "tc.h"
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
static tc_service service;
static tc_param params[1];
static tc_queue responses;
static unsigned executed;
static bool emit(void *ctx, const uint8_t *b, bool low, uint32_t now) {
    (void)ctx; (void)low;
    return tc_push(&responses, b, now, false);
}
static tc_command_result command(void *ctx, uint16_t opcode, const uint8_t *d, uint16_t n) {
    (void)ctx; (void)d; (void)n;
    if (opcode != 100) return (tc_command_result){TC_UNSUPPORTED, 0, 0};
    ++executed;
    return (tc_command_result){TC_OK, 3, executed};
}
EXPORT void sim_init(void) {
    memset(&responses, 0, sizeof responses);
    params[0] = (tc_param){1, TC_U32, 1, 25, 1, 1000};
    tc_service_init(&service, params, 1, emit, 0);
    service.control = command;
    executed = 0;
}
EXPORT void sim_receive(const uint8_t *b, uint32_t now) { tc_service_receive(&service, b, now); }
EXPORT int sim_pop(uint8_t *b, uint32_t now) { return tc_pop(&responses, b, now, 100); }
EXPORT unsigned sim_executed(void) { return executed; }
EXPORT unsigned sim_value(void) { return params[0].bits; }
EXPORT unsigned sim_duplicates(void) { return service.duplicate; }
