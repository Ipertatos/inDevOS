#include "rtl8139.h"
#include "serial.h"

void rtl8139_print_debug(void) {
    if (!rtl8139_dev.initialized) return;
    
    uint16_t cbr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_CBR);
    uint16_t capr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_CAPR);
    uint8_t cmd = rtl8139_read_reg8(&rtl8139_dev, RTL8139_CMD);
    uint16_t isr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_ISR);
    uint32_t rcr = rtl8139_read_reg32(&rtl8139_dev, RTL8139_CONFIG4); // RCR
    
    serial_printf("RTL8139 DEBUG: CBR=0x%04x CAPR=0x%04x CMD=0x%02x ISR=0x%04x RCR=0x%08x Ptr=0x%x\n",
                  cbr, capr, cmd, isr, rcr, rtl8139_dev.current_packet_ptr);
}



