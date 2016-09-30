#include <Arduino.h>
#include <avr/eeprom.h>

//#include <Wire.h>
//#include <AT24Cx.h>

#include <SPI.h>         
#include <Ethernet.h>
#include <ICMPPing.h>

#include <EEPROM.h>
#include <Wire.h>
#include <RTClib.h>

/*
CONN_CNT_LIM - после скольки попыток меняется состояние и добавляется запись в журнал
DEFAULT_STATE  - предполагаемое состояние тестируемого IP при включении
PING_OK  - лог. уровень пина при доступности узла
PING_FALSE - лог. уровень пина при потере связи 
IP_COUNT - размер банка под IP для тестов
JRN_REC_NUM - размер журнала событий
*/

// defines, compilation parameters
// (some tuning RAM and I/O )
#define CONN_CNT_LIM 5
#define DEFAULT_STATE true
#define PING_OK HIGH
#define PING_FALSE LOW
#define IP_COUNT 5
#define JRN_REC_NUM 20

// network parameters
#define DEFAULT_IP 192,168,1,215
#define DEFAULT_GW 192,168,1,1
#define DEFAULT_DNS 8,8,8,8
#define DEFAULT_MSK 255,255,255,0
#define MY_MAC 0xA6,0x80,0xFD,0x73,0x6E,0x5C

// don't change if you not sure!
#define SBUF_SZ 80
#define CFG_EEBASE 0x01
#define CFG_SIZE ( sizeof(uint8_t)*4*4 )
#define CFG_CRC ( CFG_EEBASE + CFG_SIZE )
#define IP_EEBASE ( CFG_CRC+1 )
#define IP_SIZE ( IP_COUNT*sizeof(sv_rec_t) )
#define IP_CRC ( IP_EEBASE+IP_SIZE ) 
/*
0x01: CFG_SIZE (ip[], gw[], dns[], msk[] ) = 4*4
0x11: CFG_CRC
0x12: IP_SIZE IP_COUNT*(byte ip[4] + uint16 t_out + int pin)
0x..: IP_CRC
*/

// structures, types, etc..
struct jrn_rec_t {
	uint8_t ip[4];
	bool online;
	uint32_t datetime;
} ;

struct sv_rec_t {
	uint8_t ip[4];
	uint16_t t_out;
	int pin;
} ;

struct testip_t {
//	bool use;
	sv_rec_t sv_rec;
	bool online;
	uint8_t accum;
} ;


// variables
unsigned long xtime; // time X. Lol...
char stringbuf[SBUF_SZ];  
RTC_DS1307 rtc;
//Ethernet ether;
const char daysOfTheWeek[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
testip_t ips[IP_COUNT];
jrn_rec_t journal[JRN_REC_NUM];// __attribute__ ((section(".noinit")));
int j_idx=0;
int ipIdx=0;
uint8_t cfg_ip[4] = { DEFAULT_IP };
uint8_t cfg_gw[4] = { DEFAULT_GW };
uint8_t cfg_dns[4] = { DEFAULT_DNS };
uint8_t cfg_msk[4] = { DEFAULT_MSK };
uint8_t is_dhcp=0;

// ethernet interface mac address, must be unique on the LAN
static byte mymac[] = { MY_MAC };
static uint32_t timer;

// External EEPROM memory AT24C32
//AT24Cx eemem = AT24Cx( 0x50, 4096, 32, 10);


static void printIp( IPAddress adr)
{
  Serial.print( adr[3]); Serial.print(".");
  Serial.print( adr[2]); Serial.print(".");
  Serial.print( adr[1]); Serial.print(".");
  Serial.print( adr[0]); 
}


void setup() {
int eeadr;
  // put your setup code here, to run once:
  Serial.begin(9600);
  Serial.print( F("Pinger started. Free RAM: ") );  Serial.print( freeRam() , DEC);
  Serial.print( F(" byte, max IP count: ") ); Serial.print( IP_COUNT , DEC);
  Serial.print( F(" , max journal record: ") ); Serial.println( JRN_REC_NUM , DEC);

  if (! rtc.begin()) {
    Serial.println( F("Couldn't find RTC") );
  }
  if (! rtc.isrunning()) {
    Serial.println( F("RTC is NOT running!") );	// following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  // check crc config to damage
  if ( ee_crc( CFG_EEBASE, CFG_SIZE) != EEPROM.read( CFG_CRC ) ) {
	Serial.println( F("config eeprom - damaged! use default.") );
  } else {
	Serial.println( F("config eeprom - ok"));
	eeadr = CFG_EEBASE;
	for( uint8_t i=0; i<4; i++ ) { cfg_ip[i] = EEPROM.read(eeadr++); }
	for( uint8_t i=0; i<4; i++ ) { cfg_gw[i] = EEPROM.read(eeadr++); }
	for( uint8_t i=0; i<4; i++ ) { cfg_dns[i] = EEPROM.read(eeadr++); }
	for( uint8_t i=0; i<4; i++ ) { cfg_msk[i] = EEPROM.read(eeadr++); }
  }

  // check IP list config
  if ( ee_crc( IP_EEBASE, sizeof(sv_rec_t)*IP_COUNT ) != EEPROM.read( IP_CRC ) ) {
	Serial.println(F("IP list damaged! Can't be use!"));
  } else {
	Serial.println(F("IP list ok."));
    // load IP list from EEPROM
	eeadr = IP_EEBASE;
	for( int i=0; i<IP_COUNT; i++ ) {
      eeprom_read_block( &ips[i].sv_rec, (const void*)(IP_EEBASE + i*sizeof(sv_rec_t)), sizeof(sv_rec_t));
	  ips[i].online = DEFAULT_STATE;
	  ips[i].accum = 0;
	}

  }
  // Ethernet settings 
  if (cfg_ip[0] == 0) {
    Serial.println("Waiting DHCP answer...");
    if (Ethernet.begin(mymac) == 0) {
      Serial.println( F(" DHCP failed, use default"));
      Ethernet.begin( mymac, IPAddress(cfg_ip), IPAddress(cfg_dns),  IPAddress(cfg_gw), IPAddress(cfg_msk)); // local_ip, dns_server, gateway, subnet);
    } else {
      is_dhcp = 1;
      Serial.println( F(" DCHP - ok"));
    }
  } else {
    Serial.println( F(" STATIC mode") );
    Ethernet.begin( mymac, IPAddress(cfg_ip), IPAddress(cfg_dns),  IPAddress(cfg_gw), IPAddress(cfg_msk)); // local_ip, dns_server, gateway, subnet);
  }
  
} /* end Setup */


SOCKET pingSocket = 0;
char buffer [256];
ICMPPing ping(pingSocket, (uint16_t)random(0, 255));
//IPAddress hisip(); 

void loop()
{
bool pinged;

  if ( is_dhcp !=0 ) {
    Ethernet.maintain(); // need for DHCP 
  }

	// find next used IP for ping
	if ( ips[ipIdx].sv_rec.ip[0] != 255 ) {
		IPAddress hisip( ips[ipIdx].sv_rec.ip ); 
		xtime = millis() + ips[ipIdx].sv_rec.t_out * 1000;
		pinged = false;

		//=== do network, ping is wery slow ===//
                ICMPEchoReply echoReply = ping(hisip, 2);

                if (echoReply.status == SUCCESS) {
                    pinged = true;
#if (0)
                  sprintf(buffer,
                    "Reply[%d] from: %d.%d.%d.%d: bytes=%d time=%ldms TTL=%d",
                    echoReply.data.seq,
                    echoReply.addr[0],
                    echoReply.addr[1],
                    echoReply.addr[2],
                    echoReply.addr[3],
                    REQ_DATASIZE,
                    millis() - echoReply.data.time,
                    echoReply.ttl);
                } else {
                  sprintf(buffer, "Echo request failed; %d", echoReply.status);
#endif
                }
                if (buffer[0] != 0) Serial.println(buffer);
                /*----------------------*/
		do{
			// do routine 
			if( readln( Serial, stringbuf, SBUF_SZ)== true ){
				whichcmd( stringbuf);
			}
		} while ( xtime > millis() );
		
                // begin: не меняем при переезде
		if ( pinged ) {
			if ( ips[ipIdx].online == false ) {
				ips[ipIdx].accum++;
			} else {
				ips[ipIdx].accum = 0;
			}
			pinMode(ips[ipIdx].sv_rec.pin, OUTPUT);
			digitalWrite( ips[ipIdx].sv_rec.pin, PING_OK);
		} else {
			if ( ips[ipIdx].online == true ) {
				ips[ipIdx].accum++;
			} else {
				ips[ipIdx].accum = 0;
			}
			pinMode(ips[ipIdx].sv_rec.pin, OUTPUT);
			digitalWrite( ips[ipIdx].sv_rec.pin, PING_FALSE);
		} // end:  не меняем при переезде

		// compare accum and limit
		if (ips[ipIdx].accum >= CONN_CNT_LIM ) {
			ips[ipIdx].accum = 0; 
			ips[ipIdx].online = !ips[ipIdx].online;
			Serial.print(" \"online\" to "); Serial.print( ips[ipIdx].online);
			Serial.print( F(" for address "));  printIp(ips[ipIdx].sv_rec.ip ); Serial.print("\r\n");

			// add record to journal
			j_idx++; 
			if (j_idx >=JRN_REC_NUM) j_idx=0;
			memcpy(journal[j_idx].ip, ips[ipIdx].sv_rec.ip, sizeof(uint8_t)*4 ); //ether.copyIp(journal[j_idx].ip, ips[ipIdx].sv_rec.ip);
			journal[j_idx].online = ips[ipIdx].online;
			journal[j_idx].datetime = rtc.now().unixtime();
		}
	}
	// need here also for works when list empty
	if( readln( Serial, stringbuf, SBUF_SZ)== true ){
		whichcmd( stringbuf);
	}
	//pos = ether.packetLoop(len);
	ipIdx++; 
	if (ipIdx >= IP_COUNT) ipIdx=0; 

}

/* =================================================
 * functions after this mark don't use network card,
 * but use IP in journal and settings
 * =================================================*/


bool readln( HardwareSerial &uart, char *outbuf, uint8_t buflen)
// return true when find CR, LF or both and if size limit
{
static char idx = 0;
  while (uart.available()) {
    if ( uart.peek()!= '\r' && uart.peek()!= '\n' ) {
      outbuf[ idx++ ] = uart.read();//
    } else {// если CR
      uart.read();
      if ( uart.peek()=='\n' || uart.peek()=='\r' ) uart.read();
      if ( idx == 0 ) {
        return 0;
      }
      outbuf[ idx++ ] = '\0'; // дописать 0
      idx = 0;
      return 1;
    }
    if ( idx >=(buflen-1) ) { // проверяем на длинну внутреннего буфера
      outbuf[ buflen-1 ] = '\0'; // дописать 0
      idx = 0;
      return 1;
    }
  }
  return 0;
}

void whichcmd( char *str)
// detect command
{
  if ( memcmp(str ,"statclear", 9)==0 ) {
	memset(journal, 0, sizeof(jrn_rec_t)*JRN_REC_NUM );
    Serial.println( F("ok") );
  }else if ( memcmp(str ,"statlist" ,8)==0 ) {
    funcStatlist();
  }else if ( memcmp(str ,"pinglist", 8)==0 ) {
	funcPinglist();
  }else if ( memcmp(str ,"pingset", 7)==0 ) {
	funcPingset( str);
  }else if ( memcmp(str ,"cfgshow", 7)==0 ) {
	funcCfgShow();
  }else if ( memcmp(str ,"pingdel", 7)==0 ) {
	funcPingdel( str);
  }else if ( memcmp(str ,"timeset", 7)==0 ) {
    funcTimeSet( str); // setup date and time YYYY-MM-DD hh:mm:ss
  }else if ( memcmp(str ,"cfgset", 6)==0 ) {
	funcCfgSet( str); //funcPingdel( str);
  }else if ( memcmp(str ,"time", 4)==0 ) {
    funcTime();	// print date and time from RTC 
  }else if ( memcmp(str ,"help", 4)==0 ) {
    // print short help
    Serial.println( F(" help\r\n statlist statclear\r\n pinglist pingset pingdel\r\n time timeset\r\n cfgshow cfgset") );
  }else{
    Serial.print( F("unknow cmd> "));
    Serial.println( str);
  } 
}

void funcTime( void )
{
  DateTime now = rtc.now();
  Serial.print(now.year(), DEC);
  Serial.print('-');
  Serial.print(now.month(), DEC);
  Serial.print('-');
  Serial.print(now.day(), DEC);
  Serial.print(" (");
  Serial.print(daysOfTheWeek[now.dayOfTheWeek()]);
  Serial.print(") ");
  Serial.print(now.hour(), DEC);
  Serial.print(':');
  Serial.print(now.minute(), DEC);
  Serial.print(':');
  Serial.print(now.second(), DEC);
  Serial.println();
}

void funcTimeSet( char *str)
{
  int y, m, d, hh, mm, ss;
  
  if ( sscanf_P(str, (const char *)F("%*s %d-%d-%d %d:%d:%d"), &y, &m, &d, &hh, &mm, &ss) !=6 ) {
    Serial.println( F("wrong! YYYY-MM-DD hh:mm:ss") );
    return;
  }
  rtc.adjust( DateTime( (uint16_t)y, (uint8_t)m, (uint8_t)d, (uint8_t)hh, (uint8_t)mm, (uint8_t)ss) );
  Serial.print( F("ok") );
  funcTime();
}

uint8_t static ee_crc( int eeadr, int cnt)
{
	uint8_t crc = 0;
	//Serial.print(" For len="); Serial.print( cnt, HEX);
	//Serial.print(" started from addres "); Serial.println(eeadr, HEX);
	for( ; cnt >0; cnt--) {
	  crc += EEPROM.read(eeadr++);
	}
	//Serial.print(" CRC: "); Serial.print(crc, HEX); 
	return crc;
}

static int ee_update( int eeadr, uint8_t *p, int cnt)
{
	for ( ; cnt>0; cnt--) {
//	  Serial.print(*p, HEX); Serial.print("\t");
	  if ( EEPROM.read(eeadr) != *(uint8_t *)p ) {
	    EEPROM.write( eeadr, *(uint8_t *)p );
	  }
	  eeadr++;
	  p++;
	}
        return (eeadr);
}

void funcPinglist(void)
// print IP arrays
{
char buffer[32];

	Serial.println( F("[ip adress] \t [sec]\t[pin]\t \[online]") );
	for( int idx=0; idx < IP_COUNT ; idx++) {
		if ( ips[idx].sv_rec.ip[0] != 255 ) {   // print if used
		sprintf_P(buffer, (const char *)F("%d.%d.%d.%d\t"), \
		 ips[idx].sv_rec.ip[0], ips[idx].sv_rec.ip[1], ips[idx].sv_rec.ip[2],ips[idx].sv_rec.ip[3] );
		Serial.print(buffer);
		sprintf_P(buffer, (const char *)F(" %d\t%d\t(online: %d)"), \
		 ips[idx].sv_rec.t_out, ips[idx].sv_rec.pin, ips[idx].online );
		Serial.println( buffer);
		}
	}
	Serial.println( F("ok") );
}

// add new IP with parameters if it possible
void funcPingset( char *str)
{
  unsigned char ip0, ip1, ip2, ip3; 
  int tout, pinnum;
  int idx;
  
  if ( sscanf_P(str, (const char *)F("%*s %d.%d.%d.%d %d %d"), &ip0, &ip1, &ip2, &ip3, &tout, &pinnum) !=6 ) {
    Serial.println( F("wrong! need ip, timeout, pin: \"pingset 192.168.1.200 100 5\"") );
    return;
  }
  for( idx=0; (idx < IP_COUNT && ips[idx].sv_rec.ip[0] != 255 ); idx++) { // find first unused
  }
  if ( idx < (IP_COUNT) ) {
    //ips[idx].use = true;
	ips[idx].sv_rec.ip[0] = ip0;
	ips[idx].sv_rec.ip[1] = ip1;
	ips[idx].sv_rec.ip[2] = ip2;
	ips[idx].sv_rec.ip[3] = ip3;
	if(tout <1 ) tout = 1;
	ips[idx].sv_rec.t_out = tout;
	ips[idx].sv_rec.pin = pinnum;
	ips[idx].online = true;
	ips[idx].accum = 0;

	// update EEPROM
	for( int i=0; i<IP_COUNT; i++){
	  ee_update( IP_EEBASE + i*sizeof(sv_rec_t), (uint8_t *) &ips[i].sv_rec, sizeof(sv_rec_t));
	}
	EEPROM.write(IP_CRC, ee_crc(IP_EEBASE, sizeof(sv_rec_t)*IP_COUNT) );

	Serial.println( F("ok") );
  } else {
	Serial.println( F("List is full!") );
  }
}

// delete first the same IP in array 
void funcPingdel( char *str)
{
unsigned char ip[4]; 
bool cond;
int idx;
  if ( sscanf_P(str, (const char *)F("%*s %d.%d.%d.%d %d %d"), &ip[0], &ip[1], &ip[2], &ip[3]) != 4 ) {
    Serial.println( F("wrong! need ip") );
    return;
  }
  cond = true;
  idx = 0;
  do{
	if ( !(idx < IP_COUNT) ) cond = false;
	if ( ips[idx].sv_rec.ip[0] != 255 ) {  // if used
	  if ( memcmp( ips[idx].sv_rec.ip, ip, sizeof(char)*4) == 0) {
		ips[idx].sv_rec.ip[0] = 255;  // mark as unused
		cond = false;

		// update EEPROM
		for( int i=0; i<IP_COUNT; i++){
			ee_update( IP_EEBASE + i*sizeof(sv_rec_t), (uint8_t *) &ips[i].sv_rec, sizeof(sv_rec_t));
		}
		EEPROM.write(IP_CRC, ee_crc(IP_EEBASE, sizeof(sv_rec_t)*IP_COUNT) );

	  }
	}
	idx++;
  } while ( cond );
  if ( idx < IP_COUNT) Serial.println( F("deleted ok") ); 
  else Serial.println( F("can't find") );
}


void funcStatlist()
{
char buff[16]; //192.168.100.200   2106-66-66 (Wednesday) 28:70:66
int vidx; // valid index of journal record
  for(int idx=0; idx< JRN_REC_NUM; idx++){
	vidx = ( j_idx < idx ) ? j_idx - idx + JRN_REC_NUM : idx ;
	if (journal[vidx].ip[0] != 0) {
		sprintf_P(buff, (const char *)F("%d.%d.%d.%d"), journal[vidx].ip[0],  journal[vidx].ip[1],\
		  journal[vidx].ip[2], journal[vidx].ip[3]);
		Serial.print(buff);
		if ( journal[vidx].online == true ) {
			Serial.print( F(", connect, 1, ") );
		} else {
			Serial.print( F(", disconnect, 0, ") );
		}
		DateTime evnt = DateTime( journal[vidx].datetime );
		
		sprintf_P(buff, (const char *)F("%.4d-%.2d-%.2d ("), evnt.year(), evnt.month(), evnt.day() );
		Serial.print(buff);
//		Serial.print(" (");
		Serial.print(daysOfTheWeek[evnt.dayOfTheWeek()]);
//		Serial.print(") ");
		sprintf_P(buff, (const char *)F(") %.2d:%.2d:%.2d "), evnt.hour(), evnt.minute(), evnt.second() );
		Serial.println(buff);
	}
  }
  Serial.println( F("ok") );
}


void funcCfgShow(void)
{
  Serial.print(F("IP:\t")); printIp( Ethernet.localIP() ); Serial.print("\r\n");
  Serial.print(F("GW:\t")); printIp(Ethernet.gatewayIP() ); Serial.print("\r\n");
  Serial.print(F("DNS:\t")); printIp(Ethernet.dnsServerIP() ); Serial.print("\r\n");
  Serial.print(F("MSK:\t")); printIp(Ethernet.subnetMask() ); Serial.print("\r\n");
/*  ether.printIp(F("IP:\t"), ether.myip);
  ether.printIp(F("GW:\t"), ether.gwip);
  ether.printIp(F("DNS:\t"), ether.dnsip);
  ether.printIp(F("MSK:\t"), ether.netmask); */
}

void funcCfgSet( char * str)
{
  unsigned char ip_[4];
  unsigned char gw_[4];
  unsigned char dns_[4];
  unsigned char msk_[4];
  
  if ( sscanf_P(str, (const char *)F("%*s %hhu.%hhu.%hhu.%hhu %hhu.%hhu.%hhu.%hhu %hhu.%hhu.%hhu.%hhu %hhu.%hhu.%hhu.%hhu"), \
   &ip_[3], &ip_[2], &ip_[1], &ip_[0],  &gw_[3], &gw_[2], &gw_[1], &gw_[0], \
   &dns_[3], &dns_[2], &dns_[1], &dns_[0],  &msk_[3], &msk_[2], &msk_[1], &msk_[0]) == 16) {
#if (0)
  Serial.println (str);
  printIp(ip_);
  printIp(gw_);
  printIp(dns_);
  printIp(msk_);
#endif
    if ( ip_[0] == 0) {
	  // use DHCP
    Serial.println("Waiting DHCP answer...");
    if ( Ethernet.begin(mymac)==0 )
        Serial.println(F("DHCP failed"));
    } else {
	  // use static settings
      Ethernet.begin( mymac, IPAddress(cfg_ip), IPAddress(cfg_dns),  IPAddress(cfg_gw), IPAddress(cfg_msk) ); //ether.staticSetup(ip_, gw_, dns_, msk_);
    } 
  int eeadr = CFG_EEBASE;
  eeadr = ee_update( eeadr, ip_, 4);// eeadr += 4;
  eeadr = ee_update( eeadr, gw_, 4);// eeadr += 4;
  eeadr = ee_update( eeadr, dns_, 4);// eeadr += 4;
  ee_update( eeadr, msk_, 4);// eeadr += 4;
  EEPROM.write( CFG_CRC, ee_crc( CFG_EEBASE, 4*4) );
  Serial.println(F("ok."));

  } else {
    Serial.println(F("wrong! type \"cfgset IP GW DNS MSK\" with nnn.nnn.nnn.nnn\r\n Use 0.*.*.0 for DHCP."));
  }
}


static inline int freeRam () {
// print free RAM memory
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}
 
