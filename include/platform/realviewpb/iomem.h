#ifndef __PLATFORM_IOMEM_H__
#define __PLATFORM_IOMEM_H__

// UART0
// TODO: VBASE SHOULDN'T BE HARDCODED!
#define R_UART0_PBASE (0x10009000)
#define R_UART0_VBASE (0xFFFF0000)

#define O_UART0_DR (0x0)
#define R_UART0_DR (R_UART0_VBASE+O_UART0_DR)

#define O_UART0_FR (0x18)
#define R_UART0_FR (R_UART0_VBASE+O_UART0_FR)

#define O_UART0_IMSC (0x038)
#define R_UART0_IMSC (R_UART0_VBASE+O_UART0_IMSC)

// GIC0
#define R_GIC0_PBASE (0x1E000000)
#define R_GIC0_VBASE (0xFFFF1000)

#endif // __PLATFORM_IOMEM_H__
