#ifndef MOCK_STM32F1XX_H
#define MOCK_STM32F1XX_H
#include <stdint.h>
/* Minimal register double for driver logic; not a bus/timing simulator. */
typedef struct {uint32_t TIR,TDTR,TDLR,TDHR;} MockTx;
typedef struct {uint32_t RIR,RDTR,RDLR,RDHR;} MockRx;
typedef struct {uint32_t MCR,MSR,TSR,ESR,BTR,FMR,FA1R,FS1R,FM1R,FFA1R,RF0R;
MockTx sTxMailBox[3]; MockRx sFIFOMailBox[2];} MockCan;
typedef struct {uint32_t APB1ENR,APB2ENR;} MockRcc;
typedef struct {uint32_t MAPR;} MockAfio;
typedef struct {uint32_t CRL,CRH,ODR;} MockGpio;
extern MockCan mock_can; extern MockRcc mock_rcc; extern MockAfio mock_afio;
extern MockGpio mock_gpio;
#define CAN1 (&mock_can)
#define RCC (&mock_rcc)
#define AFIO (&mock_afio)
#define GPIOA (&mock_gpio)
#define GPIOB (&mock_gpio)
#define GPIOD (&mock_gpio)
/* bxCAN completion layout from ST CMSIS STM32F103xB: RQCP=0, TXOK=1, ALST=2, TERR=3. */
#define CAN_TSR_RQCP0 (1u<<0)
#define CAN_TSR_TXOK0 (1u<<1)
#define CAN_TSR_ALST0 (1u<<2)
#define CAN_TSR_TERR0 (1u<<3)
#define RCC_APB2ENR_IOPBEN (1u<<3)
#define RCC_APB2ENR_AFIOEN (1u<<0)
#define RCC_APB2ENR_IOPDEN (1u<<5)
#define RCC_APB1ENR_CAN1EN (1u<<25)
#define AFIO_MAPR_CAN_REMAP (3u<<13)
#define AFIO_MAPR_CAN_REMAP_REMAP2 (2u<<13)
#define AFIO_MAPR_CAN_REMAP_REMAP3 (3u<<13)
#define CAN_MCR_INRQ 1u
#define CAN_MCR_SLEEP 2u
#define CAN_MCR_ABOM (1u<<6)
#define CAN_MSR_INAK 1u
#define CAN_FMR_FINIT 1u
#define CAN_FA1R_FACT0 1u
#define CAN_FS1R_FSC0 1u
#define CAN_FM1R_FBM0 1u
#define CAN_FFA1R_FFA0 1u
#define CAN_RF0R_FMP0 3u
#define CAN_RF0R_RFOM0 (1u<<5)
#define CAN_TI0R_TXRQ 1u
#define CAN_ESR_BOFF (1u<<2)
#endif
