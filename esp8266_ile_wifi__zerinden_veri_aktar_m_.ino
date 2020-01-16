#include <ESP8266WiFi.h>

#include <algorithm> // std::min
//wifi işlemleri
#ifndef STASSID
#define STASSID "your-ssid" //wifi ismi
#define STAPSK  "your-password" //2ifi şifresi
#endif

#define SWAP_PINS 0

#define SERIAL_LOOPBACK 0

#define BAUD_SERIAL 115200
#define BAUD_LOGGER 115200
#define RXBUFFERSIZE 1024

#if SERIAL_LOOPBACK
#undef BAUD_SERIAL
#define BAUD_SERIAL 3000000
#include <esp8266_peri.h>
#endif


#if SWAP_PINS
#include <SoftwareSerial.h>
SoftwareSerial* logger = nullptr;
#else
#define logger (&Serial1)
#endif


#define STACK_PROTECTOR  512 // bytes

//server a kaç kişi bağlı olabilir 
#define MAX_SRV_CLIENTS 2
const char* ssid = STASSID;
const char* password = STAPSK;

//port
const int port = 23;

WiFiServer server(port);
WiFiClient serverClients[MAX_SRV_CLIENTS];
void setup() {

  Serial.begin(BAUD_SERIAL);
  Serial.setRxBufferSize(RXBUFFERSIZE);

#if SWAP_PINS
  Serial.swap();
// Donanım seri şimdi RX'te: GPIO13 TX: GPIO15 
// günlük kaydı için SoftwareSerial'ı normal RX (3) / TX (1) üzerinde kullanın
  logger = new SoftwareSerial(3, 1);
  logger->begin(BAUD_LOGGER);
  logger->enableIntTx(false);
  logger->println("\n\nGünlük kaydı için SoftwareSerial kullanma");
#else
  logger->begin(BAUD_LOGGER);
  logger->println("\n\nGünlük kaydı için SoftwareSerial kullanma");
#endif
  logger->println(ESP.getFullVersion());
  logger->printf("Serial baud: %d (8n1: %d KB/s)\n", BAUD_SERIAL, BAUD_SERIAL * 8 / 10 / 1024);
  logger->printf("Seri alma tampon boyutu: %d bytes\n", RXBUFFERSIZE);

#if SERIAL_LOOPBACK
  USC0(0) |= (1 << UCLBE); // tamamlanmamış HardwareSerial API
  logger->println("Seri Dahili Geri Döngü etkin");
#endif

  //Wifi işelmleri wifi server ın kurulması 
  //ve aynı zaamanda wifi ismi ve şifresinin girilerek bağlanması
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  logger->print("\nBağlanıyor ");
  logger->println(ssid);
  //wifi ye bağlanmamış ise 
  while (WiFi.status() != WL_CONNECTED) {
    logger->print('.');
    delay(500);
  }
  logger->println();
  logger->print("bağlı, adres=");
  // wifi de ki kullandıgı ip hakkında ekrana bilgi basar 
  logger->println(WiFi.localIP());

  //Server ın başlatılması
  server.begin();
  server.setNoDelay(true);

  logger->print("Hazır! 'Telnet' kullanın");
  logger->print(WiFi.localIP());
  logger->printf(" %d' to connect\n", port);
}

void loop() {
  //yeni müşteri olup olmadığını kontrol et
  //yani server başlatılmış ise 
  if (server.hasClient()) {
    //serbest / bağlantısız nokta bul
    int i;
    for (i = 0; i < MAX_SRV_CLIENTS; i++)
      if (!serverClients[i]) { // ! sunucu İstemcileri [i] .connected () ile eşdeğerdir
        serverClients[i] = server.available();
        logger->print("New client: index ");
        logger->print(i);
        break;
      }

    //boş / bağlantısız nokta yok, bu yüzden reddet
    if (i == MAX_SRV_CLIENTS) {
      server.available().println("busy");
      // ipuçları: server.available () kısa süreli kapsamı olan bir WiFiClient
      // kapsam dışı olduğunda, bir WiFiClient
      // - flush () - tüm veriler gönderilecek
      // - stop () - otomatik olarak da
      logger->printf("sunucu% d etkin bağlantıyla meşgul\n", MAX_SRV_CLIENTS);
    }
  }

  //TCP istemcilerinde veri olup olmadığını kontrol edin
#if 1
  // İnanılmaz bir şekilde, bu kod aşağıdaki tamponlu koddan daha hızlı - # 4620 gerekli
  // geri döngü / 3000000 baud ortalaması 348 KB / s
  for (int i = 0; i < MAX_SRV_CLIENTS; i++)
    while (serverClients[i].available() && Serial.availableForWrite() > 0) {
      // char ile çalışmak char çok verimli değil
      Serial.write(serverClients[i].read());
    }
#else
  // geri döngü / 3000000 baud ortalaması: 312 KB / s
  for (int i = 0; i < MAX_SRV_CLIENTS; i++)
    while (serverClients[i].available() && Serial.availableForWrite() > 0) {
      size_t maxToSerial = std::min(serverClients[i].available(), Serial.availableForWrite());
      maxToSerial = std::min(maxToSerial, (size_t)STACK_PROTECTOR);
      uint8_t buf[maxToSerial];
      size_t tcp_got = serverClients[i].read(buf, maxToSerial);
      size_t serial_sent = Serial.write(buf, tcp_got);
      if (serial_sent != maxToSerial) {
        logger->printf("len mismatch: available:%zd tcp-read:%zd serial-write:%zd\n", maxToSerial, tcp_got, serial_sent);
      }
    }
#endif

  // maksimum çıktı boyutunu belirleme "adil TCP kullanımı"
  // ! client.connected () olduğunda client.availableForWrite () 0 döndürür
  size_t maxToTcp = 0;
  for (int i = 0; i < MAX_SRV_CLIENTS; i++)
    if (serverClients[i]) {
      size_t afw = serverClients[i].availableForWrite();
      if (afw) {
        if (!maxToTcp) {
          maxToTcp = afw;
        } else {
          maxToTcp = std::min(maxToTcp, afw);
        }
      } else {
        // sıkışık müşterileri uyar ama yoksay
        //fazşşa bir bağlantı var ise bu bölüme girilerek
        logger->println("bir müşteri tıkalı");
      }
    }

  //check UART for data
  size_t len = std::min((size_t)Serial.available(), maxToTcp);
  len = std::min(len, (size_t)STACK_PROTECTOR);
  if (len) {
    uint8_t sbuf[len];
    size_t serial_got = Serial.readBytes(sbuf, len);
    // UART verilerini bağlı tüm telnet istemcilerine aktarma
    for (int i = 0; i < MAX_SRV_CLIENTS; i++)
      // client.availableForWrite () 0 ise (sıkışık)
      // ve o zamandan beri arttı,
      // yazma alanının yeterli olduğundan emin olun:
      if (serverClients[i].availableForWrite() >= serial_got) {
        size_t tcp_sent = serverClients[i].write(sbuf, serial_got);
        if (tcp_sent != len) {
          logger->printf("len mismatch: available:%zd serial-read:%zd tcp-write:%zd\n", len, serial_got, tcp_sent);
        }
      }
  }
}
