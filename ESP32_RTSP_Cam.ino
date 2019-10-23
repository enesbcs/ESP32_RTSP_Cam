// WARNING!!! Make sure that you have either selected ESP32 Wrover Module,
//            or another board which has PSRAM enabled
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <AutoConnect.h> // Needs AutoConnect, PageBuilder libraries!
#include "soc/soc.h" //disable brownout problems
#include "soc/rtc_cntl_reg.h"  //disable brownout problems

#define ENABLE_RTSPSERVER
#define ENABLE_WEBSERVER
#define RESOLUTION  FRAMESIZE_XGA // FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
#define QUALITY        10          // JPEG quality 10-63 (lower means better quality)
#define PIN_FLASH_LED  4           // GPIO4 for AIThinker module, set to -1 if not needed!

#include "OV2640.h"
#ifdef ENABLE_RTSPSERVER
 #include "OV2640Streamer.h"
 #include "CRtspSession.h"
#endif

OV2640 cam;
boolean useRTSP = true;

WebServer server(80);
AutoConnect Portal(server);
AutoConnectConfig acConfig;

#ifdef ENABLE_RTSPSERVER
WiFiServer rtspServer(554);
CStreamer *streamer;
#endif

void setflash(byte state) {
  if (PIN_FLASH_LED>-1){
    digitalWrite(PIN_FLASH_LED, state);
  }   
}

void handle_jpg_stream(void)
{
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (1)
  {
    cam.run();
    if (!client.connected())
      break;
    response = "--frame\r\n";
    response += "Content-Type: image/jpeg\r\n\r\n";
    server.sendContent(response);

    client.write((char *)cam.getfb(), cam.getSize());
    server.sendContent("\r\n");
    if (!client.connected())
      break;
  }
}

void handle_jpg(void)
{
  setflash(1);
  WiFiClient client = server.client();
  cam.run();
  if (!client.connected())
  {
    return;
  }
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-disposition: inline; filename=snapshot.jpg\r\n";
  response += "Content-type: image/jpeg\r\n\r\n";
  server.sendContent(response);
  client.write((char *)cam.getfb(), cam.getSize());
  setflash(0);
}

void handle_root(void)
{
  
  String reply = F("<!doctype html><html><H1>ESP32CAM</H1>");

  reply += F("<p>Resolution: ");

  switch(RESOLUTION) {
    case FRAMESIZE_QVGA:
     reply += F("QVGA (320x240)");
    break;
    case FRAMESIZE_CIF:
     reply += F("CIF (400x296)");
    break;
    case FRAMESIZE_VGA:
     reply += F("VGA (640x480)");
    break;
    case FRAMESIZE_SVGA:
     reply += F("SVGA (800x600)");
    break;
    case FRAMESIZE_XGA:
     reply += F("XGA (1024x768)");
    break;
    case FRAMESIZE_SXGA:
     reply += F("SXGA (1280x1024)");
    break;
    case FRAMESIZE_UXGA:
     reply += F("UXGA (1600x1200)");
    break;
    default:
     reply += F("Unknown");
    break;
  }
  
  reply += F("<p>PSRAM found: ");
  if (psramFound()){
   reply += F("TRUE");
  } else {
   reply += F("FALSE");
  }
  String url = "http://" + WiFi.localIP().toString() + "/snapshot.jpg";
  reply += "<p>Snapshot URL:<br> <a href='" + url + "'>" + url + "</a>";
  url = "http://" + WiFi.localIP().toString() + "/mjpg";
  reply += "<p>HTTP MJPEG URL:<br> <a href='" + url + "'>" + url + "</a>";
  url = "rtsp://" + WiFi.localIP().toString() + ":554/mjpeg/1";
  reply += "<p>RTSP MJPEG URL:<br> " + url;
  url = "http://" + WiFi.localIP().toString() + "/_ac";
  reply += "<p>WiFi settings page:<br> <a href='" + url + "'>" + url + "</a>";

  reply += F("</body></html>");
  server.send(200, "text/html", reply);
}

void handleNotFound()
{
  String message = "Server is running!\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  server.send(200, "text/plain", message);
}

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector 
  Serial.begin(115200);
  //  while (!Serial){;} // debug only
  camera_config_t cconfig;
  cconfig = esp32cam_aithinker_config;
  if (psramFound()) {
    cconfig.frame_size = RESOLUTION; // FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    cconfig.jpeg_quality = QUALITY;
    cconfig.fb_count = 2;
  } else {
    if (RESOLUTION>FRAMESIZE_SVGA) {
     cconfig.frame_size = FRAMESIZE_SVGA;
    }
    cconfig.jpeg_quality = 12;
    cconfig.fb_count = 1;
  }
  if (PIN_FLASH_LED>-1){
    pinMode(PIN_FLASH_LED, OUTPUT);
    setflash(0);
  }  
  cam.init(cconfig);

  server.on("/", handle_root);
  server.on("/mjpg", HTTP_GET, handle_jpg_stream);
  server.on("/snapshot.jpg", HTTP_GET, handle_jpg);
  Portal.onNotFound(handleNotFound);
  acConfig.apid = "ESP-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
  acConfig.psk = "configesp";
//  acConfig.apip = IPAddress(192,168,4,1);
  acConfig.hostName = acConfig.apid;
  acConfig.autoReconnect = true;
  Portal.config(acConfig);
  Portal.begin();

#ifdef ENABLE_RTSPSERVER
  if (useRTSP) {
    rtspServer.begin();
    streamer = new OV2640Streamer(cam);             // our streamer for UDP/TCP based RTP transport
  }
#endif
}

void loop()
{
  //server.handleClient();
  Portal.handleClient();

#ifdef ENABLE_RTSPSERVER
  if (useRTSP) {
    uint32_t msecPerFrame = 100;
    static uint32_t lastimage = millis();

    // If we have an active client connection, just service that until gone
    streamer->handleRequests(0); // we don't use a timeout here,
    // instead we send only if we have new enough frames
    uint32_t now = millis();
    if (streamer->anySessions()) {
      if (now > lastimage + msecPerFrame || now < lastimage) { // handle clock rollover
        streamer->streamImage(now);
        lastimage = now;

        // check if we are overrunning our max frame rate
        now = millis();
        if (now > lastimage + msecPerFrame) {
          printf("warning exceeding max frame rate of %d ms\n", now - lastimage);
        }
      }
    }

    WiFiClient rtspClient = rtspServer.accept();
    if (rtspClient) {
      Serial.print("client: ");
      Serial.print(rtspClient.remoteIP());
      Serial.println();
      streamer->addSession(rtspClient);
    }
  }
#endif
}
