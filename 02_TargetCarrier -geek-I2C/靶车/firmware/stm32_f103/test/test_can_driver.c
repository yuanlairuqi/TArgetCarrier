#include <stdio.h>
#include <string.h>
#include "stm32f1xx.h"
#include "can_drv.h"
MockCan mock_can; MockRcc mock_rcc; MockAfio mock_afio; MockGpio mock_gpio;
#define CHECK(x) do {if (!(x)) {printf("line %d failed\n",__LINE__);return 1;}} while(0)
int main(void) {
    uint8_t bytes[8]={1,2,3,4,5,6,7,8};
    const uint32_t tme=1u<<26;
    mock_can.TSR=tme;
    CHECK(can_drv_send(1,bytes,8));
    mock_can.TSR=tme|CAN_TSR_RQCP0|CAN_TSR_TXOK0;
    CHECK(!can_drv_send(2,bytes,8)); /* Completed result must be collected before reuse. */
    can_tx_done_t done=can_drv_poll_tx();
    CHECK(done.completed==1 && done.ok==1 && done.failed==0);
    CHECK(mock_can.TSR==CAN_TSR_RQCP0); /* Driver writes the actual W1C bit. */
    mock_can.TSR=tme;
    CHECK(can_drv_send(1,bytes,8));
    mock_can.TSR=tme|CAN_TSR_RQCP0|CAN_TSR_TERR0;
    done=can_drv_poll_tx();
    CHECK(done.completed==1 && done.ok==0 && done.failed==1); /* RQCP is not TXOK. */
    mock_can.TSR=tme;
    CHECK(can_drv_send(1,bytes,8));
    mock_can.TSR=tme;
    done=can_drv_poll_tx(); CHECK(done.completed==0); /* Empty without RQCP is not completion. */
    mock_can.TSR=0;
    done=can_drv_poll_tx(); CHECK(done.completed==0); /* Still in flight. */
    can_drv_abort_all(); CHECK(mock_can.TSR==0x00808080u);
    CHECK(can_drv_bus_ok_count()==1 && can_drv_bus_fail_count()==1);
    puts("CAN register adapter regressions passed");return 0;
}
