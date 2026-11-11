#ifndef STM_MOCK
#define STM_MOCK
#include <stdint.h>
#ifndef I2C_CCR_FS
#define I2C_CCR_FS (1UL<<15)
#endif
#define DMA_ISR_TEIF2 (1UL << 7)
typedef struct {
    volatile uint32_t CRL;
    volatile uint32_t CRH;
    volatile uint32_t BSRR;
    volatile uint32_t APB2ENR;
    volatile uint32_t APB1ENR;
    volatile uint32_t AHBENR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t SR;
    volatile uint32_t SR1;
    volatile uint32_t SR2;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CCR;
    volatile uint32_t CPAR;
    volatile uint32_t CMAR;
    volatile uint32_t CNDTR;
    volatile uint32_t IFCR;
    volatile uint32_t ISR;
    volatile uint32_t PR;
    volatile uint32_t RTSR;
    volatile uint32_t IMR;
    volatile uint32_t EXTICR[4];
    volatile uint32_t OAR1;
    volatile uint32_t TRISE;
} Reg;
typedef Reg GPIO_TypeDef;
static Reg mock_GPIOA;
#define GPIOA (&mock_GPIOA)
static Reg mock_GPIOB;
#define GPIOB (&mock_GPIOB)
static Reg mock_RCC;
#define RCC (&mock_RCC)
static Reg mock_DMA1;
#define DMA1 (&mock_DMA1)
static Reg mock_DMA1_Channel2;
#define DMA1_Channel2 (&mock_DMA1_Channel2)
static Reg mock_DMA1_Channel3;
#define DMA1_Channel3 (&mock_DMA1_Channel3)
static Reg mock_SPI1;
#define SPI1 (&mock_SPI1)
static Reg mock_USART3;
#define USART3 (&mock_USART3)
static Reg mock_I2C2;
#define I2C2 (&mock_I2C2)
static Reg mock_AFIO;
#define AFIO (&mock_AFIO)
static Reg mock_EXTI;
#define EXTI (&mock_EXTI)
#define DMA1_Channel3_IRQn 0
#define EXTI4_IRQn 1
#define I2C2_EV_IRQn 2
#define I2C2_ER_IRQn 3
static inline uint32_t __get_PRIMASK(void) {
    return 0;
}
static inline void __disable_irq(void) {
}
static inline void __set_PRIMASK(uint32_t p) {
    (void)p;
}
static inline void NVIC_SetPriority(int a, int b) {
    (void)a;
    (void)b;
}
static inline void NVIC_EnableIRQ(int a) {
    (void)a;
}
#define RCC_APB2ENR_IOPBEN RCC_APB2ENR_IOPBEN_Msk
#define RCC_APB2ENR_IOPBEN_Msk (0x1UL << RCC_APB2ENR_IOPBEN_Pos)
#define RCC_APB2ENR_IOPBEN_Pos (3U)
#define DMA_CCR_CIRC DMA_CCR_CIRC_Msk
#define DMA_CCR_CIRC_Msk (0x1UL << DMA_CCR_CIRC_Pos)
#define DMA_CCR_CIRC_Pos (5U)
#define SPI_CR2_RXDMAEN SPI_CR2_RXDMAEN_Msk
#define SPI_CR2_RXDMAEN_Msk (0x1UL << SPI_CR2_RXDMAEN_Pos)
#define SPI_CR2_RXDMAEN_Pos (0U)
#define USART_CR1_RE USART_CR1_RE_Msk
#define USART_CR1_RE_Msk (0x1UL << USART_CR1_RE_Pos)
#define USART_CR1_RE_Pos (2U)
#define I2C_SR1_RXNE I2C_SR1_RXNE_Msk
#define I2C_SR1_RXNE_Msk (0x1UL << I2C_SR1_RXNE_Pos)
#define I2C_SR1_RXNE_Pos (6U)
#define I2C_SR1_ARLO I2C_SR1_ARLO_Msk
#define I2C_SR1_ARLO_Msk (0x1UL << I2C_SR1_ARLO_Pos)
#define I2C_SR1_ARLO_Pos (9U)
#define DMA_IFCR_CGIF2 DMA_IFCR_CGIF2_Msk
#define DMA_IFCR_CGIF2_Msk (0x1UL << DMA_IFCR_CGIF2_Pos)
#define DMA_IFCR_CGIF2_Pos (4U)
#define USART_CR3_DMAR USART_CR3_DMAR_Msk
#define USART_CR3_DMAR_Msk (0x1UL << USART_CR3_DMAR_Pos)
#define USART_CR3_DMAR_Pos (6U)
#define RCC_APB2ENR_SPI1EN RCC_APB2ENR_SPI1EN_Msk
#define RCC_APB2ENR_SPI1EN_Msk (0x1UL << RCC_APB2ENR_SPI1EN_Pos)
#define RCC_APB2ENR_SPI1EN_Pos (12U)
#define I2C_CR1_PE I2C_CR1_PE_Msk
#define I2C_CR1_PE_Msk (0x1UL << I2C_CR1_PE_Pos)
#define I2C_CR1_PE_Pos (0U)
#define RCC_APB1ENR_I2C2EN RCC_APB1ENR_I2C2EN_Msk
#define RCC_APB1ENR_I2C2EN_Msk (0x1UL << RCC_APB1ENR_I2C2EN_Pos)
#define RCC_APB1ENR_I2C2EN_Pos (22U)
#define DMA_CCR_MINC DMA_CCR_MINC_Msk
#define DMA_CCR_MINC_Msk (0x1UL << DMA_CCR_MINC_Pos)
#define DMA_CCR_MINC_Pos (7U)
#define SPI_SR_RXNE SPI_SR_RXNE_Msk
#define SPI_SR_RXNE_Msk (0x1UL << SPI_SR_RXNE_Pos)
#define SPI_SR_RXNE_Pos (0U)
#define I2C_SR1_AF I2C_SR1_AF_Msk
#define I2C_SR1_AF_Msk (0x1UL << I2C_SR1_AF_Pos)
#define I2C_SR1_AF_Pos (10U)
#define USART_SR_ORE USART_SR_ORE_Msk
#define USART_SR_ORE_Msk (0x1UL << USART_SR_ORE_Pos)
#define USART_SR_ORE_Pos (3U)
#define DMA_CCR_TEIE DMA_CCR_TEIE_Msk
#define DMA_CCR_TEIE_Msk (0x1UL << DMA_CCR_TEIE_Pos)
#define DMA_CCR_TEIE_Pos (3U)
#define I2C_CR2_ITBUFEN I2C_CR2_ITBUFEN_Msk
#define I2C_CR2_ITBUFEN_Msk (0x1UL << I2C_CR2_ITBUFEN_Pos)
#define I2C_CR2_ITBUFEN_Pos (10U)
#define I2C_CR1_SWRST I2C_CR1_SWRST_Msk
#define I2C_CR1_SWRST_Msk (0x1UL << I2C_CR1_SWRST_Pos)
#define I2C_CR1_SWRST_Pos (15U)
#define I2C_SR1_STOPF I2C_SR1_STOPF_Msk
#define I2C_SR1_STOPF_Msk (0x1UL << I2C_SR1_STOPF_Pos)
#define I2C_SR1_STOPF_Pos (4U)
#define I2C_CR1_ACK I2C_CR1_ACK_Msk
#define I2C_CR1_ACK_Msk (0x1UL << I2C_CR1_ACK_Pos)
#define I2C_CR1_ACK_Pos (10U)
#define I2C_SR1_TXE I2C_SR1_TXE_Msk
#define I2C_SR1_TXE_Msk (0x1UL << I2C_SR1_TXE_Pos)
#define I2C_SR1_TXE_Pos (7U)
#define USART_CR1_UE USART_CR1_UE_Msk
#define USART_CR1_UE_Msk (0x1UL << USART_CR1_UE_Pos)
#define USART_CR1_UE_Pos (13U)
#define RCC_APB2ENR_IOPAEN RCC_APB2ENR_IOPAEN_Msk
#define RCC_APB2ENR_IOPAEN_Msk (0x1UL << RCC_APB2ENR_IOPAEN_Pos)
#define RCC_APB2ENR_IOPAEN_Pos (2U)
#define I2C_SR1_ADDR I2C_SR1_ADDR_Msk
#define I2C_SR1_ADDR_Msk (0x1UL << I2C_SR1_ADDR_Pos)
#define I2C_SR1_ADDR_Pos (1U)
#define I2C_SR1_OVR I2C_SR1_OVR_Msk
#define I2C_SR1_OVR_Msk (0x1UL << I2C_SR1_OVR_Pos)
#define I2C_SR1_OVR_Pos (11U)
#define RCC_APB2ENR_AFIOEN RCC_APB2ENR_AFIOEN_Msk
#define RCC_APB2ENR_AFIOEN_Msk (0x1UL << RCC_APB2ENR_AFIOEN_Pos)
#define RCC_APB2ENR_AFIOEN_Pos (0U)
#define RCC_AHBENR_DMA1EN RCC_AHBENR_DMA1EN_Msk
#define RCC_AHBENR_DMA1EN_Msk (0x1UL << RCC_AHBENR_DMA1EN_Pos)
#define RCC_AHBENR_DMA1EN_Pos (0U)
#define I2C_CR2_ITEVTEN I2C_CR2_ITEVTEN_Msk
#define I2C_CR2_ITEVTEN_Msk (0x1UL << I2C_CR2_ITEVTEN_Pos)
#define I2C_CR2_ITEVTEN_Pos (9U)
#define USART_SR_FE USART_SR_FE_Msk
#define USART_SR_FE_Msk (0x1UL << USART_SR_FE_Pos)
#define USART_SR_FE_Pos (1U)
#define DMA_CCR_DIR DMA_CCR_DIR_Msk
#define DMA_CCR_DIR_Msk (0x1UL << DMA_CCR_DIR_Pos)
#define DMA_CCR_DIR_Pos (4U)
#define SPI_CR1_SPE SPI_CR1_SPE_Msk
#define SPI_CR1_SPE_Msk (0x1UL << SPI_CR1_SPE_Pos)
#define SPI_CR1_SPE_Pos (6U)
#define I2C_SR2_TRA I2C_SR2_TRA_Msk
#define I2C_SR2_TRA_Msk (0x1UL << I2C_SR2_TRA_Pos)
#define I2C_SR2_TRA_Pos (2U)
#define USART_CR3_DMAT USART_CR3_DMAT_Msk
#define USART_CR3_DMAT_Msk (0x1UL << USART_CR3_DMAT_Pos)
#define USART_CR3_DMAT_Pos (7U)
#define I2C_SR1_BERR I2C_SR1_BERR_Msk
#define I2C_SR1_BERR_Msk (0x1UL << I2C_SR1_BERR_Pos)
#define I2C_SR1_BERR_Pos (8U)
#define DMA_ISR_TCIF3 DMA_ISR_TCIF3_Msk
#define DMA_ISR_TCIF3_Msk (0x1UL << DMA_ISR_TCIF3_Pos)
#define DMA_ISR_TCIF3_Pos (9U)
#define USART_SR_NE USART_SR_NE_Msk
#define USART_SR_NE_Msk (0x1UL << USART_SR_NE_Pos)
#define USART_SR_NE_Pos (2U)
#define I2C_SR2_BUSY I2C_SR2_BUSY_Msk
#define I2C_SR2_BUSY_Msk (0x1UL << I2C_SR2_BUSY_Pos)
#define I2C_SR2_BUSY_Pos (1U)
#define DMA_CCR_TCIE DMA_CCR_TCIE_Msk
#define DMA_CCR_TCIE_Msk (0x1UL << DMA_CCR_TCIE_Pos)
#define DMA_CCR_TCIE_Pos (1U)
#define USART_CR1_TE USART_CR1_TE_Msk
#define USART_CR1_TE_Msk (0x1UL << USART_CR1_TE_Pos)
#define USART_CR1_TE_Pos (3U)
#define RCC_APB1ENR_USART3EN RCC_APB1ENR_USART3EN_Msk
#define RCC_APB1ENR_USART3EN_Msk (0x1UL << RCC_APB1ENR_USART3EN_Pos)
#define RCC_APB1ENR_USART3EN_Pos (18U)
#define DMA_CCR_EN DMA_CCR_EN_Msk
#define DMA_CCR_EN_Msk (0x1UL << DMA_CCR_EN_Pos)
#define DMA_CCR_EN_Pos (0U)
#define DMA_IFCR_CGIF3 DMA_IFCR_CGIF3_Msk
#define DMA_IFCR_CGIF3_Msk (0x1UL << DMA_IFCR_CGIF3_Pos)
#define DMA_IFCR_CGIF3_Pos (8U)
#define SPI_SR_OVR SPI_SR_OVR_Msk
#define SPI_SR_OVR_Msk (0x1UL << SPI_SR_OVR_Pos)
#define SPI_SR_OVR_Pos (6U)
#define I2C_CR2_ITERREN I2C_CR2_ITERREN_Msk
#define I2C_CR2_ITERREN_Msk (0x1UL << I2C_CR2_ITERREN_Pos)
#define I2C_CR2_ITERREN_Pos (8U)
#define SPI_CR2_TXDMAEN SPI_CR2_TXDMAEN_Msk
#define SPI_CR2_TXDMAEN_Msk (0x1UL << SPI_CR2_TXDMAEN_Pos)
#define SPI_CR2_TXDMAEN_Pos (1U)
#define DMA_ISR_TEIF3 DMA_ISR_TEIF3_Msk
#define DMA_ISR_TEIF3_Msk (0x1UL << DMA_ISR_TEIF3_Pos)
#define DMA_ISR_TEIF3_Pos (11U)
#define DMA_CCR_PL_1 (0x2UL << DMA_CCR_PL_Pos)
#define DMA_CCR_PL_Pos (12U)
#endif
