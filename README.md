# multipinger
Arduino pinger to few adress, each  with LED indiactor.

 Require [RTClib](https://github.com/jcw/rtclib) DS1307 (TinyRTC), *Ethernet* W5100, [ICMPPing](https://github.com/mcherry/ICMPPing/tree/master/ICMPPing) libs and *ATMega328*

It managed by terminal commands:
* **help** - print this list.
* **statlist** - print statistic.
* **statclear** - clear statistic.
* **pinglist** - list IP and its pin and timeout.
* **pingset** *nnn.mmm.xx.yy timeout pinumber*- add IP to list for check. Timeout in sec.
* **pingclear** - remove one IP from list. If exsist few - removed first.
* **time** - show curent time
* **timeset** *YYYY-MM-DD hh:mm:ss* - set time and date
* **cfgshow** - print curent network parameters from Ethternet shield (not a saved settings).
* **cfgset** *ip3.ip2.ip1.ip0 gw3.gw2.gw1.gw0 dns3.dns2.dns1.dns0 msk3.msk2.msk1.msk0* - set network parameters. If *ip0* == 0 trying use DNS, else - static address.

*#define*s at the begin sketch tune count IPs, journal record, active pin level, MAC and default network parameters.

Configuration store in internal EEPROM and protected checksum.
Remove *reset-en* jumper to prevent reset board when terminal open (and reset statistic).

### ToDo:
 Move journal to AT24C32 external EEPROM.