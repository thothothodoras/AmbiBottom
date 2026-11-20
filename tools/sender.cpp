// Throwaway e2e sender: DTLS-PSK client streaming a CONSTANT 3-lamp HueStream
// v1 XY frame at 25 fps for N seconds - i.e. what the TV does on a still scene.
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/timing.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
static std::vector<uint8_t> unhex(const std::string& h){std::vector<uint8_t> v;for(size_t i=0;i+1<h.size();i+=2)v.push_back((uint8_t)strtol(h.substr(i,2).c_str(),nullptr,16));return v;}
int main(int argc,char**argv){
  std::string id=argv[1]; auto key=unhex(argv[2]); const char* port=argv[3]; int secs=atoi(argv[4]);
  mbedtls_net_context fd; mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_timing_delay_context timer;
  mbedtls_net_init(&fd);mbedtls_entropy_init(&ent);mbedtls_ctr_drbg_init(&drbg);mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&conf);
  mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&ent,(const unsigned char*)"s",1);
  const char* host=getenv("SENDER_HOST")?getenv("SENDER_HOST"):"127.0.0.1"; /* v0.36: SENDER_HOST = a real ESP */
  if(mbedtls_net_connect(&fd,host,port,MBEDTLS_NET_PROTO_UDP)){puts("connect fail");return 1;}
  mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_DATAGRAM,MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&conf,MBEDTLS_SSL_VERIFY_NONE);mbedtls_ssl_conf_rng(&conf,mbedtls_ctr_drbg_random,&drbg);mbedtls_ssl_conf_read_timeout(&conf,2000);
  mbedtls_ssl_conf_psk(&conf,key.data(),key.size(),(const unsigned char*)id.data(),id.size());
  mbedtls_ssl_setup(&ssl,&conf);mbedtls_ssl_set_timer_cb(&ssl,&timer,mbedtls_timing_set_delay,mbedtls_timing_get_delay);
  mbedtls_ssl_set_bio(&ssl,&fd,mbedtls_net_send,mbedtls_net_recv,mbedtls_net_recv_timeout);
  int r; do{r=mbedtls_ssl_handshake(&ssl);}while(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE);
  if(r){printf("handshake fail %d\n",r);return 1;}
  puts("handshake ok");
  std::vector<uint8_t> f={'H','u','e','S','t','r','e','a','m',1,0,0,0,0,1,0};
  auto lamp=[&](int id,uint16_t x,uint16_t y,uint16_t b){f.push_back(0);f.push_back(id>>8);f.push_back(id&255);for(uint16_t v:{x,y,b}){f.push_back(v>>8);f.push_back(v&255);}};
  // v0.24: SENDER_LAMPS=5 -> five distinct lamps 1..5 (for end_lamps)
  if(getenv("SENDER_LAMPS")&&atoi(getenv("SENDER_LAMPS"))==5){lamp(1,0xB000,0x5000,0x8000);lamp(2,0x6000,0x4000,0x8000);lamp(3,0x3000,0x9000,0x8000);lamp(4,0x2000,0x1800,0x8000);lamp(5,0x5000,0x5500,0xFFFF);}
  else if(argc>5){lamp(3,0x2000,0x1800,0x8000);lamp(1,0x6000,0x4000,0x8000);lamp(2,0x3000,0x9000,0x8000);}else{lamp(1,0x6000,0x4000,0x8000);lamp(2,0x3000,0x9000,0x8000);lamp(3,0x2000,0x1800,0x8000);}
  auto end=std::chrono::steady_clock::now()+std::chrono::seconds(secs);int n=0;
  while(std::chrono::steady_clock::now()<end){f[11]=(uint8_t)n; mbedtls_ssl_write(&ssl,f.data(),f.size());n++;std::this_thread::sleep_for(std::chrono::milliseconds(40));}
  printf("sent %d identical frames\n",n);mbedtls_ssl_close_notify(&ssl);return 0;}
