#include <avr/pgmspace.h>
#include <avr/boot.h>
#include "board.h"
#include "ethernet.h"
#include "fncollection.h"
#include "stringfunc.h"
#include "timer.h"
#include "display.h"
#include "delay.h"
#include "ntp.h"

#include "uip_arp.h"
#include "drivers/interfaces/network.h"
#include "drivers/enc28j60/enc28j60.h"
#include "apps/dhcpc/dhcpc.h"
#include "delay.h"

#define BUF ((struct uip_eth_hdr *)uip_buf)
struct timer periodic_timer, arp_timer;
static struct uip_eth_addr mac;       // static for dhcpc
uint8_t eth_debug = 0;


#define bsbg boot_signature_byte_get

void
ethernet_init(void)
{
  // reset Ethernet
  ENC28J60_RESET_DDR  |= _BV( ENC28J60_RESET_BIT );
  ENC28J60_RESET_PORT &= ~_BV( ENC28J60_RESET_BIT );

  my_delay_ms( 200 );

  // unreset Ethernet
  ENC28J60_RESET_PORT |= _BV( ENC28J60_RESET_BIT );

  my_delay_ms( 200 );
  network_init();
  
  // setup two periodic timers
  timer_set(&periodic_timer, CLOCK_SECOND / 2);
  timer_set(&arp_timer, CLOCK_SECOND * 10);
  
  uip_init();
  
  mac.addr[0] = erb(EE_MAC_ADDR+0);
  mac.addr[1] = erb(EE_MAC_ADDR+1);
  mac.addr[2] = erb(EE_MAC_ADDR+2);
  mac.addr[3] = erb(EE_MAC_ADDR+3);
  mac.addr[4] = erb(EE_MAC_ADDR+4);
  mac.addr[5] = erb(EE_MAC_ADDR+5);
  uip_setethaddr(mac);
  network_set_MAC(mac.addr);

  if(erb(EE_USE_DHCP)) {
    enc28j60PhyWrite(PHLCON,0x4A6);// LED A: Link Status  LED B: Blink slow
    dhcpc_init(mac.addr, 6);

  } else {
    uip_ipaddr_t ipaddr;
    erip(ipaddr, EE_IP4_ADDR);    uip_sethostaddr(ipaddr);
    erip(ipaddr, EE_IP4_GATEWAY); uip_setdraddr(ipaddr);
    erip(ipaddr, EE_IP4_NETMASK); uip_setnetmask(ipaddr);
    enc28j60PhyWrite(PHLCON,0x476);// LED A: Link Status  LED B: TX/RX

  }
    
  tcplink_init();
  ntp_gmtoff = erb(EE_IP4_NTPOFFSET);
}

void
ethernet_reset(void)
{
  char buf[21];

  buf[1] = 'i';
  buf[2] = 'd'; strcpy_P(buf+3, PSTR("1"));             write_eeprom(buf);//DHCP
  buf[2] = 'a'; strcpy_P(buf+3, PSTR("192.168.0.244")); write_eeprom(buf);//IP
  buf[2] = 'n'; strcpy_P(buf+3, PSTR("255.255.255.0")); write_eeprom(buf);
  buf[2] = 'g'; strcpy_P(buf+3, PSTR("192.168.0.1"));   write_eeprom(buf);//GW
  buf[2] = 'p'; strcpy_P(buf+3, PSTR("2323"));          write_eeprom(buf);
  buf[2] = 'N'; strcpy_P(buf+3, PSTR("0.0.0.0"));       write_eeprom(buf);//==GW
  buf[2] = 'o'; strcpy_P(buf+3, PSTR("00"));            write_eeprom(buf);//GMT

  // Generate a "unique" MAC address from the unique serial number
  buf[2] = 'm'; strcpy_P(buf+3, PSTR("000425"));        // Atmel MAC Range
  tohex(bsbg(0x0e)+bsbg(0x10), (uint8_t*)buf+9);
  tohex(bsbg(0x12)+bsbg(0x14), (uint8_t*)buf+11);
  tohex(bsbg(0x16)+bsbg(0x18), (uint8_t*)buf+13);
  buf[15] = 0;
  write_eeprom(buf);
}

void
eth_func(char *in)
{
  if(in[1] == 'i') {
    ethernet_init();

  } else if(in[1] == 'd') {
    eth_debug = (eth_debug+1) & 0x3;
    DH2(eth_debug);
    DNL();

  }
}

void
dumppkt(void)
{
  uint8_t *a = uip_buf;

  DC('e');DC(' ');
  DU(uip_len,5);

  output_enabled &= ~OUTPUT_TCP;
  DC(' '); DC('d'); DC(':');
  for(uint8_t i = 0; i < sizeof(struct uip_eth_addr); i++)
    DH2(*a++);
  DC(' '); DC('s'); DC(':');
  for(uint8_t i = 0; i < sizeof(struct uip_eth_addr); i++)
    DH2(*a++);

  DC(' '); DC('t'); DC(':');
  DH2(*a++);
  DH2(*a++);
  DNL();

  if(eth_debug > 2)
    dumpmem(a, uip_len - sizeof(struct uip_eth_hdr));
  output_enabled |= OUTPUT_TCP;
}

void
Ethernet_Task(void)
{
  int i;
  
  uip_len = network_read();

  if(uip_len > 0) {

    if(eth_debug > 1)
      dumppkt();

    if(BUF->type == htons(UIP_ETHTYPE_IP)){
      uip_arp_ipin();
      uip_input();
      if(uip_len > 0) {
        uip_arp_out();
        network_send();
      }
    } else if(BUF->type == htons(UIP_ETHTYPE_ARP)){
      uip_arp_arpin();
      if(uip_len > 0){
        network_send();
      }
    }
    
  } else if(timer_expired(&periodic_timer)) {
    timer_reset(&periodic_timer);
    
    for(i = 0; i < UIP_CONNS; i++) {
      uip_periodic(i);
      if(uip_len > 0) {
        uip_arp_out();
        network_send();
      }
    }
    
    for(i = 0; i < UIP_UDP_CONNS; i++) {
      uip_udp_periodic(i);
      if(uip_len > 0) {
        uip_arp_out();
        network_send();
      }
    }
       
    if(timer_expired(&arp_timer)) {
      timer_reset(&arp_timer);
      uip_arp_timer();
         
    }
  }

}

void                             // EEPROM Read IP
erip(void *ip, uint8_t *addr)
{
  uip_ipaddr(ip, erb(addr), erb(addr+1), erb(addr+2), erb(addr+3));
}

static void                             // EEPROM Write IP
ewip(const u16_t ip[2], uint8_t *addr)
{
  uint16_t ip0 = HTONS(ip[0]);
  uint16_t ip1 = HTONS(ip[1]);
  ewb(addr+0, ip0>>8);
  ewb(addr+1, ip0&0xff);
  ewb(addr+2, ip1>>8);
  ewb(addr+3, ip1&0xff);
}

void
dhcpc_configured(const struct dhcpc_state *s)
{
  ewip(s->ipaddr, EE_IP4_ADDR);            uip_sethostaddr(s->ipaddr);
  ewip(s->default_router, EE_IP4_GATEWAY); uip_setdraddr(s->default_router);
  ewip(s->netmask, EE_IP4_NETMASK);        uip_setnetmask(s->netmask);
  //resolv_conf(s->dnsaddr);
  enc28j60PhyWrite(PHLCON,0x476);// LED A: Link Status  LED B: TX/RX
  uip_udp_remove(s->conn);
}

void
tcp_appcall()
{
  if(uip_conn->lport == tcplink_port)
    tcplink_appcall();
}

void
udp_appcall()
{
  static uint8_t dhcp_state = PT_WAITING;

  if(dhcp_state != PT_ENDED) {
    dhcp_state = handle_dhcp();
    if(dhcp_state == PT_ENDED)
      ntp_sendpacket();

  } else if(uip_udp_conn &&
            uip_newdata() &&
            uip_udp_conn->lport == HTONS(NTP_PORT)) {
    ntp_digestpacket();

  } else if(timer_expired(&ntp_timer)) {
    ntp_sendpacket();

  }
}
