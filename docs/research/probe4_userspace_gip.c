/* Research probe (AnyPad-era gip.c API; not built): the capture
 * docs/research/gip_bringup_capture.log came from it, against a 3537:1004
 * dock. GIP bring-up with async never-cancelled reads (libusb on macOS clears the
 * endpoint halt on every cancelled/timeout transfer, which upsets pads). */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gip.h"
static long T0;
static long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000;}
void log_line(const char*f,...){va_list a;va_start(a,f);printf("[%6ld] ",ms()-T0);vprintf(f,a);printf("\n");fflush(stdout);va_end(a);}
static void hexl(const char*t,const uint8_t*p,int n){char b[400];int k=0;for(int i=0;i<n&&k<390;i++)k+=sprintf(b+k,"%02x ",p[i]);log_line("%s (%d): %s",t,n,b);}
static libusb_device_handle*H; static gip_t G; static pad_state ST; static int GONE; static unsigned long FR; static uint32_t LASTB=0xffffffff; static int LASTLX=-1;
static void LIBUSB_CALL out_cb(struct libusb_transfer*t){ if(t->status!=LIBUSB_TRANSFER_COMPLETED) log_line("tx failed status %d",t->status); }
static int snd(void*c,const uint8_t*p,int n){(void)c; struct libusb_transfer*t=libusb_alloc_transfer(0); uint8_t*b=malloc(n); memcpy(b,p,n);
  libusb_fill_interrupt_transfer(t,H,0x02,b,n,out_cb,NULL,1000); t->flags=LIBUSB_TRANSFER_FREE_BUFFER|LIBUSB_TRANSFER_FREE_TRANSFER; return libusb_submit_transfer(t)==0;}
static void LIBUSB_CALL in_cb(struct libusb_transfer*t){
  if(t->status==LIBUSB_TRANSFER_COMPLETED){ long now=ms()-T0; FR++; if(FR<=25) hexl("rx",t->buffer,t->actual_length);
    int r=gip_receive(&G,t->buffer,t->actual_length,now,&ST,snd,NULL);
    if((r&GIP_RX_INPUT)&&(ST.buttons!=LASTB||abs((int)ST.lx-LASTLX)>40)){LASTB=ST.buttons;LASTLX=ST.lx;log_line("  INPUT buttons=%08x lx=%u ly=%u rx=%u ry=%u lt=%u rt=%u",ST.buttons,ST.lx,ST.ly,ST.rx,ST.ry,ST.l2,ST.r2);}
    if(libusb_submit_transfer(t)) GONE=1;
  } else { log_line("rx status %d", t->status); if(t->status==LIBUSB_TRANSFER_NO_DEVICE||t->status==LIBUSB_TRANSFER_ERROR) GONE=1; else if(libusb_submit_transfer(t)) GONE=1; }
}
static void windows_desc(libusb_device_handle*h){
  uint8_t os[18]={0},c[64]={0}; int r=libusb_control_transfer(h,0x80,0x06,0x03EE,0,os,18,500); log_line("  0xEE -> %d vendor code %02x",r,r>=18?os[16]:0);
  if(r>=18){ r=libusb_control_transfer(h,0xC0,os[16],0,4,c,16,500); int L=r>=4?(c[0]|c[1]<<8):0; if(L>16&&L<=64) r=libusb_control_transfer(h,0xC0,os[16],0,4,c,L,500); log_line("  compat ID -> %d (%.8s)",r,(char*)c+18);} }
int main(int argc,char**argv){
  T0=ms(); int windesc = argc>1 && atoi(argv[1]); int secs = argc>2?atoi(argv[2]):25;
  libusb_context*c; libusb_init(&c);
  log_line("strategy: %s, async reads. waiting for 3537:1004 (replug the pad)", windesc?"windows-descriptors":"plain");
  long t=ms(); while(ms()-t<180000 && !(H=libusb_open_device_with_vid_pid(c,0x3537,0x1004))) usleep(2000);
  if(!H){log_line("no 1004");return 1;}
  long topen=ms(); int cur=-1; libusb_get_configuration(H,&cur); log_line("opened, config %d",cur);
  if(windesc) windows_desc(H);
  if(cur!=1) log_line("set_configuration -> %s",libusb_error_name(libusb_set_configuration(H,1)));
  int r=libusb_claim_interface(H,0); log_line("claim -> %s",libusb_error_name(r)); if(r) return 1;
  pad_state_reset(&ST); gip_init(&G,"dock",0x3537,0x1004,0,ms()-T0);
  for(int i=0;i<4;i++){struct libusb_transfer*x=libusb_alloc_transfer(0); libusb_fill_interrupt_transfer(x,H,0x82,malloc(64),64,in_cb,NULL,0); if(libusb_submit_transfer(x)) log_line("submit failed");}
  G.reads=1;
  while(!GONE && ms()-topen<secs*1000){ struct timeval tv={0,10000}; libusb_handle_events_timeout_completed(c,&tv,NULL); gip_tick(&G,ms()-T0,snd,NULL); }
  log_line("RESULT %s: %s after %ld ms, frames %lu, state %s, hellos %lu, statuses %lu, inputs %lu", windesc?"windows-descriptors":"plain", GONE?"DEVICE GONE":"still connected", ms()-topen, FR, gip_state_name(G.state), G.hellos, G.statuses, G.inputs);
  if(!GONE && (G.inputs||G.statuses)) log_line("SUCCESS");
  return 0;
}
