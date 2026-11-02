#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>
#include <memory>
#include <atomic>
#include "detector.h"
#include "smtp_session.h"
#include "smtp_ca.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

using namespace soundalert;
constexpr size_t kClipSamples = kRate*8, kSlots=4;
struct Clip {
  int16_t* pcm;
  size_t count;
  uint64_t due,end;
  time_t when;
  float score;
  char id[64];
};
struct Record { long long when=0; unsigned long long end=0; float score=0; unsigned attempts=0; long long next=0; };
Clip clips[kSlots];
QueueHandle_t freeClips,readyClips;
SemaphoreHandle_t sdMutex;
std::unique_ptr<Detector> detector;
int16_t* audioRing;
uint64_t samples=0;
uint32_t sequence=0;
char bootId[40];
std::atomic<uint32_t> detectedCount{0},storedCount{0},acceptedCount{0},uncertainCount{0},overflowCount{0};
std::atomic<bool> manualTrigger{false};

class SdGuard { public: SdGuard(){xSemaphoreTake(sdMutex,portMAX_DELAY);} ~SdGuard(){xSemaphoreGive(sdMutex);} };
bool validClock(){return time(nullptr)>1700000000;}
const char* trustedCA(){return strlen(config::SMTP_ROOT_CA)?config::SMTP_ROOT_CA:mailtrust::root;}
void fatal(const char* message){Serial.printf("FATAL: %s\n",message); while(true) delay(1000);}
String basePath(const String& id){return "/queue/"+id;}
String fileId(String path){int slash=path.lastIndexOf('/'); if(slash>=0) path=path.substring(slash+1); int dot=path.lastIndexOf('.'); return dot>=0?path.substring(0,dot):path;}
bool fileExists(const String& path){SdGuard lock;return SD.exists(path);}
bool renameState(const String& id,const char* from,const char* to){SdGuard lock;return SD.rename(basePath(id)+from,basePath(id)+to);}
void recovery() {
  SdGuard lock;
  File folder=SD.open("/queue");
  for(File f=folder.openNextFile();f;f=folder.openNextFile()) {
    String name=f.name(); f.close();
    if(name.endsWith(".sending")) {
      String id=fileId(name);
      // If power failed while moving a definite retry, prefer uncertainty over duplicate mail.
      if(SD.exists(basePath(id)+".ready"))SD.remove(basePath(id)+".ready");
      SD.rename(basePath(id)+".sending",basePath(id)+".uncertain");
      Serial.printf("Recovered uncertain event %s\n",id.c_str());
    }
  }
  folder.close();
}
void wavHeader(uint8_t* h,uint32_t dataSize) {
  memset(h,0,44);
  auto u16=[&](int p,uint16_t v){h[p]=v;h[p+1]=v>>8;};
  auto u32=[&](int p,uint32_t v){for(int i=0;i<4;++i)h[p+i]=v>>(8*i);};
  memcpy(h,"RIFF",4);u32(4,dataSize+36);memcpy(h+8,"WAVEfmt ",8);u32(16,16);
  u16(20,1);u16(22,1);u32(24,kRate);u32(28,kRate*2);u16(32,2);u16(34,16);
  memcpy(h+36,"data",4);u32(40,dataSize);
}
bool storeClip(const Clip& clip) {
  SdGuard lock;
  if(SD.totalBytes()-SD.usedBytes()<uint64_t(config::MIN_FREE_MB)*1024*1024) return false;
  String path=basePath(clip.id);
  if(SD.exists(path+".ready"))return true;
  if(!SD.exists(path+".wav")){
    File f=SD.open(path+".wav.tmp",FILE_WRITE);
    if(!f) return false;
    uint8_t header[44];wavHeader(header,clip.count*2);
    bool ok=f.write(header,44)==44;
    for(size_t p=0;ok && p<clip.count*2;p+=4096) {
      size_t n=std::min(size_t(4096),clip.count*2-p);
      ok=f.write(reinterpret_cast<const uint8_t*>(clip.pcm)+p,n)==n;
      vTaskDelay(1);
    }
    f.flush();f.close();
    if(!ok || !SD.rename(path+".wav.tmp",path+".wav")) return false;
  }else{
    File existing=SD.open(path+".wav");
    bool valid=existing && existing.size()==44+clip.count*2;existing.close();
    if(!valid)return false;
  }
  File meta=SD.open(path+".meta.tmp",FILE_WRITE);
  if(!meta) return false;
  size_t n=meta.printf("%lld %llu %.8f 0 0\n",(long long)clip.when,(unsigned long long)clip.end,clip.score);
  meta.flush();meta.close();
  return n>0 && SD.rename(path+".meta.tmp",path+".ready");
}
void storeTask(void*) {
  for(;;) {
    Clip* clip=nullptr;
    if(xQueueReceive(readyClips,&clip,portMAX_DELAY)==pdTRUE) {
      // Keep this complete snapshot in RAM on a storage error; capture never blocks.
      while(!storeClip(*clip)) {
        Serial.printf("STORAGE_FAULT pending=%s; free space/card check needed\n",clip->id);
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
      ++storedCount;
      Serial.printf("QUEUED %s samples=%u\n",clip->id,unsigned(clip->count));
      xQueueSend(freeClips,&clip,portMAX_DELAY);
    }
  }
}
void beginCapture(uint64_t end,float score,Clip** pending) {
  ++detectedCount;
  Clip* clip=nullptr;
  if(xQueueReceive(freeClips,&clip,0)!=pdTRUE) {
    ++overflowCount;Serial.println("CAPTURE_OVERFLOW: no free recording slot; event audio not saved");return;
  }
  snprintf(clip->id,sizeof(clip->id),"%s-%06lu",bootId,(unsigned long)++sequence);
  clip->end=end;clip->due=end+kRate;clip->when=validClock()?time(nullptr):0;clip->score=score;
  for(size_t i=0;i<kSlots;++i)if(!pending[i]){pending[i]=clip;break;}
  Serial.printf("DETECTED %s score=%.4f; recording post-roll\n",clip->id,score);
}
void captureTask(void*) {
  int32_t raw[kBlock]; int16_t pcm[kBlock];
  size_t bytes=0;
  Clip* pending[kSlots]={};
  for(;;) {
    size_t got=0;
    esp_err_t result=i2s_read(I2S_NUM_0,reinterpret_cast<uint8_t*>(raw)+bytes,sizeof(raw)-bytes,&got,pdMS_TO_TICKS(200));
    if(result!=ESP_OK){Serial.printf("I2S_READ_ERROR %d\n",int(result));vTaskDelay(1);continue;}
    bytes+=got;
    if(bytes<sizeof(raw)) continue;
    bytes=0;
    for(size_t i=0;i<kBlock;++i){
      int32_t v=raw[i]>>config::MIC_SAMPLE_SHIFT;
      pcm[i]=int16_t(std::max(-32768,std::min(32767,int(v))));
      audioRing[samples%kClipSamples]=pcm[i];++samples;
    }
    for(size_t i=0;i<kSlots;++i) {
      Clip* c=pending[i];
      if(c && samples>=c->due){
        c->count=std::min(uint64_t(kClipSamples),samples);
        size_t start=(samples-c->count)%kClipSamples;
        size_t first=std::min(c->count,kClipSamples-start);
        memcpy(c->pcm,audioRing+start,first*2);
        memcpy(c->pcm+first,audioRing,(c->count-first)*2);
        if(xQueueSend(readyClips,&c,0)!=pdTRUE)fatal("snapshot queue invariant");
        pending[i]=nullptr;
      }
    }
    Detection d=detector->push(pcm);
    bool manual=manualTrigger.exchange(false);
    if(d.fired || manual)beginCapture(d.endSample,manual?-1.0f:d.score,pending);
  }
}

bool loadRecord(const String& id,Record& r) {
  SdGuard lock;
  File f=SD.open(basePath(id)+".ready"); if(!f)return false;
  String line=f.readStringUntil('\n');f.close();
  return sscanf(line.c_str(),"%lld %llu %f %u %lld",&r.when,&r.end,&r.score,&r.attempts,&r.next)==5;
}
String nextReady() {
  SdGuard lock;
  String best;
  File folder=SD.open("/queue");
  for(File f=folder.openNextFile();f;f=folder.openNextFile()){
    String name=f.name();f.close();
    if(!name.endsWith(".ready"))continue;
    String id=fileId(name);
    File meta=SD.open(basePath(id)+".ready");
    String line=meta.readStringUntil('\n');meta.close();
    Record r;
    if(sscanf(line.c_str(),"%lld %llu %f %u %lld",&r.when,&r.end,&r.score,&r.attempts,&r.next)!=5){
      Serial.printf("INVALID_METADATA %s\n",id.c_str());continue;
    }
    if(r.next>(long long)time(nullptr))continue;
    if(best.isEmpty()||id<best)best=id;
  }
  folder.close();return best;
}
bool scheduleRetry(const String& id,Record& r) {
  ++r.attempts;
  unsigned delaySeconds=std::min(900u,30u*(1u<<std::min(r.attempts,5u)));
  r.next=(long long)time(nullptr)+delaySeconds;
  SdGuard lock;
  File f=SD.open(basePath(id)+".retry.tmp",FILE_WRITE);if(!f)return false;
  bool ok=f.printf("%lld %llu %.8f %u %lld\n",r.when,r.end,r.score,r.attempts,r.next)>0;
  f.flush();f.close();
  if(!ok||!SD.rename(basePath(id)+".retry.tmp",basePath(id)+".ready"))return false;
  return SD.remove(basePath(id)+".sending");
}
class SecureMailIO:public MailIO {
 public:
  WiFiClientSecure client;
  bool write(const char* data,size_t n)override{
    uint32_t start=millis();size_t p=0;
    while(p<n){
      if(!client.connected() || millis()-start>30000)return false;
      size_t sent=client.write(reinterpret_cast<const uint8_t*>(data+p),n-p);
      if(!sent){vTaskDelay(1);continue;}p+=sent;
    }
    return true;
  }
  int reply(uint32_t timeout)override{
    uint32_t start=millis();String line;int expected=-1;size_t lines=0;
    while(millis()-start<timeout){
      if(!client.available()){
        if(!client.connected())return -1;vTaskDelay(pdMS_TO_TICKS(2));continue;
      }
      int ch=client.read();if(ch<0)continue;
      if(ch=='\n'){
        if(line.length()<4 || !isDigit(line[0]) || !isDigit(line[1]) || !isDigit(line[2]))return -1;
        int code=line.substring(0,3).toInt();
        if(expected!=-1 && expected!=code)return -1;
        if(line[3]==' ')return code;
        if(line[3]!='-' || ++lines>100)return -1;
        expected=code;line="";
      }else if(ch!='\r'){
        if(line.length()>=1024)return -1;line+=char(ch);
      }
    }
    return -1;
  }
  ~SecureMailIO(){client.stop();}
};
class SdAttachment:public Attachment {
 public:
  File file;size_t length;
  explicit SdAttachment(const String& path){SdGuard lock;file=SD.open(path);length=file?file.size():0;}
  size_t size()const override{return length;}
  size_t read(uint8_t* b,size_t n)override{SdGuard lock;return file?file.read(b,n):0;}
  ~SdAttachment(){SdGuard lock;if(file)file.close();}
};
std::string utcDate(time_t when,const char* format){
  tm t;gmtime_r(&when,&t);char out[96];strftime(out,sizeof(out),format,&t);return out;
}
Delivery sendEvent(const String& id,const Record& record) {
  if(!strlen(trustedCA()))return Delivery::PermanentFailure;
  if(!strlen(config::SMTP_ROOT_CA)&&strcmp(config::SMTP_HOST,mailtrust::host))return Delivery::PermanentFailure;
  SecureMailIO io;
  io.client.setCACert(trustedCA());
  io.client.setHandshakeTimeout(30);io.client.setTimeout(15);
  if(!io.client.connect(config::SMTP_HOST,config::SMTP_PORT,30000))return Delivery::Retry;
  SdAttachment attachment(basePath(id)+".wav");
  if(!attachment.length)return Delivery::PermanentFailure;
  Envelope mail;
  mail.from=config::MAIL_FROM;mail.to=config::MAIL_TO;mail.user=config::SMTP_USER;mail.password=config::SMTP_AUTH_CODE;
  mail.eventId=id.c_str();
  mail.subject="声音提醒 "+mail.eventId;
  mail.date=utcDate(time(nullptr),"%a, %d %b %Y %H:%M:%S +0000");
  std::string occurrence=record.when?utcDate(time_t(record.when)+8*3600,"%Y-%m-%d %H:%M:%S UTC+08:00"):"时钟未同步；请参考设备编号和启动后采样时间";
  mail.body="检测到一次新的目标音乐。每个不同事件编号代表一次新检测。\r\n事件编号："+mail.eventId+
    "\r\n检测时间："+occurrence+"\r\n启动后秒数："+std::to_string(double(record.end)/kRate)+
    "\r\n匹配得分："+std::to_string(record.score)+"\r\n此前重试次数："+std::to_string(record.attempts)+
    "\r\n发信时间："+mail.date+"\r\n附件为本次现场录音。相同事件编号的邮件只对应同一件事情。\r\n";
  return smtpSend(io,attachment,mail);
}
void senderTask(void*) {
  for(;;){
    if(!config::MAIL_ENABLED || !strlen(config::SMTP_USER) || !strlen(config::MAIL_TO) ||
       !strlen(config::MAIL_FROM) || !strlen(config::SMTP_AUTH_CODE) || !strlen(trustedCA()) ||
       WiFi.status()!=WL_CONNECTED || !validClock()){
      vTaskDelay(pdMS_TO_TICKS(1000));continue;
    }
    String id=nextReady();
    if(id.isEmpty()){vTaskDelay(pdMS_TO_TICKS(1000));continue;}
    Record r;
    if(!loadRecord(id,r)||!renameState(id,".ready",".sending")){vTaskDelay(pdMS_TO_TICKS(1000));continue;}
    auto result=sendEvent(id,r);
    bool saved=false;
    if(result==Delivery::Accepted){
      saved=renameState(id,".sending",".accepted");++acceptedCount;
      if(saved && !config::KEEP_ACCEPTED_RECORDINGS){
        SdGuard lock;
        if(SD.remove(basePath(id)+".wav"))SD.remove(basePath(id)+".accepted");
      }
    }else if(result==Delivery::Uncertain){
      saved=renameState(id,".sending",".uncertain");++uncertainCount;
    }else if(result==Delivery::PermanentFailure){
      saved=renameState(id,".sending",".failed");
    }else saved=scheduleRetry(id,r);
    Serial.printf("MAIL_STATE %s result=%d state_saved=%d\n",id.c_str(),int(result),int(saved));
    if(!saved)Serial.println("STATE_WRITE_FAULT: inspect SD; event remains marked sending");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
void cleanupAccepted(){
  SdGuard lock;
  File folder=SD.open("/queue");size_t removed=0;
  for(File f=folder.openNextFile();f;f=folder.openNextFile()){
    String name=f.name();f.close();if(!name.endsWith(".accepted"))continue;
    String id=fileId(name);
    // Explicit local command only; never delete queued/uncertain/failed recordings.
    if(SD.remove(basePath(id)+".wav") && SD.remove(basePath(id)+".accepted"))++removed;
  }
  folder.close();Serial.printf("Removed %u accepted event recordings\n",unsigned(removed));
}
static WiFiMulti wifiMulti;
void wifiTask(void*){
  for(;;){
    if(!strlen(config::WIFI_SSID)&&!strlen(config::WIFI_SSID_2)){vTaskDelay(pdMS_TO_TICKS(10000));continue;}
    if(WiFi.status()!=WL_CONNECTED){
      wifiMulti.run(8000);
      Serial.printf("WIFI state=%d ip=%s\n",int(WiFi.status()),WiFi.localIP().toString().c_str());
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
void setup(){
  Serial.begin(115200);delay(1000);
  Serial.println("Sound alert: TRACK01 template, independent notifications, no five-minute stop.");
  if(!psramFound())fatal("8MB PSRAM required; verify board model and PlatformIO settings");
  audioRing=static_cast<int16_t*>(heap_caps_malloc(kClipSamples*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!audioRing)fatal("audio ring allocation failed");
  freeClips=xQueueCreate(kSlots,sizeof(Clip*));readyClips=xQueueCreate(kSlots,sizeof(Clip*));sdMutex=xSemaphoreCreateMutex();
  if(!freeClips||!readyClips||!sdMutex)fatal("queue/mutex allocation failed");
  for(auto& clip:clips){
    clip.pcm=static_cast<int16_t*>(heap_caps_malloc(kClipSamples*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!clip.pcm)fatal("snapshot allocation failed");Clip* ptr=&clip;xQueueSend(freeClips,&ptr,0);
  }
  detector.reset(new Detector(config::MATCH_THRESHOLD));
  uint64_t mac=ESP.getEfuseMac();
  snprintf(bootId,sizeof(bootId),"%06llX-%08lX%08lX",(unsigned long long)(mac&0xffffff),(unsigned long)esp_random(),(unsigned long)esp_random());
  SPI.begin(config::SD_SCK,config::SD_MISO,config::SD_MOSI,config::SD_CS);
  if(!SD.begin(config::SD_CS,SPI,4000000)||SD.cardType()==CARD_NONE)fatal("microSD missing; no persistent event storage");
  if(!SD.exists("/queue")&&!SD.mkdir("/queue"))fatal("cannot create queue directory");
  recovery();
  i2s_config_t ic={};
  ic.mode=i2s_mode_t(I2S_MODE_MASTER|I2S_MODE_RX);ic.sample_rate=kRate;ic.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  ic.channel_format=config::MIC_RIGHT_CHANNEL?I2S_CHANNEL_FMT_ONLY_RIGHT:I2S_CHANNEL_FMT_ONLY_LEFT;
  ic.communication_format=I2S_COMM_FORMAT_STAND_I2S;
  ic.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;ic.dma_buf_count=8;ic.dma_buf_len=256;ic.use_apll=false;
  i2s_pin_config_t pins={};pins.bck_io_num=config::MIC_BCLK;pins.ws_io_num=config::MIC_WS;pins.data_out_num=I2S_PIN_NO_CHANGE;pins.data_in_num=config::MIC_DATA;pins.mck_io_num=I2S_PIN_NO_CHANGE;
  if(i2s_driver_install(I2S_NUM_0,&ic,0,nullptr)!=ESP_OK||i2s_set_pin(I2S_NUM_0,&pins)!=ESP_OK)fatal("I2S initialization failed");
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.setSleep(false);
  if(strlen(config::WIFI_SSID))wifiMulti.addAP(config::WIFI_SSID,config::WIFI_PASSWORD);
  if(strlen(config::WIFI_SSID_2))wifiMulti.addAP(config::WIFI_SSID_2,config::WIFI_PASSWORD_2);
  configTime(0,0,"ntp.aliyun.com","pool.ntp.org");
  if(xTaskCreatePinnedToCore(captureTask,"capture",16384,nullptr,3,nullptr,1)!=pdPASS ||
     xTaskCreatePinnedToCore(storeTask,"storage",8192,nullptr,1,nullptr,0)!=pdPASS ||
     xTaskCreatePinnedToCore(senderTask,"sender",20480,nullptr,1,nullptr,0)!=pdPASS ||
     xTaskCreatePinnedToCore(wifiTask,"wifi",6144,nullptr,1,nullptr,0)!=pdPASS)fatal("task creation failed");
  Serial.printf("mail_enabled=%d; commands: s=status, t=manual test event, c=delete accepted local clips\n",config::MAIL_ENABLED);
}
void loop(){
  if(Serial.available()){
    char c=Serial.read();
    if(c=='t')manualTrigger=true;
    if(c=='c')cleanupAccepted();
    if(c=='s')Serial.printf("events=%lu stored=%lu smtp_accepted=%lu uncertain=%lu overflow=%lu wifi=%d clock=%d mail_enabled=%d psram_free=%u\n",
      (unsigned long)detectedCount,(unsigned long)storedCount,(unsigned long)acceptedCount,(unsigned long)uncertainCount,(unsigned long)overflowCount,
      int(WiFi.status()),int(validClock()),int(config::MAIL_ENABLED),ESP.getFreePsram());
  }
  static uint32_t reconnectAt=0;
  if(strlen(config::WIFI_SSID)&&WiFi.status()!=WL_CONNECTED&&millis()-reconnectAt>30000){reconnectAt=millis();WiFi.reconnect();}
  delay(10);
}
