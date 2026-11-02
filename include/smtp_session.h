#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include "delivery.h"

namespace soundalert {
struct MailIO {
  virtual bool write(const char*,size_t)=0;
  virtual int reply(uint32_t timeoutMs)=0;
  virtual ~MailIO()=default;
};
struct Attachment {
  virtual size_t size() const=0;
  virtual size_t read(uint8_t*,size_t)=0;
  virtual ~Attachment()=default;
};
struct Envelope {
  std::string from,to,user,password,eventId,subject,body,date;
};
inline std::string base64(const uint8_t* data,size_t n) {
  static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out; out.reserve((n+2)/3*4);
  for(size_t i=0;i<n;i+=3) {
    uint32_t v=uint32_t(data[i])<<16;
    if(i+1<n) v|=uint32_t(data[i+1])<<8;
    if(i+2<n) v|=data[i+2];
    out+=alphabet[(v>>18)&63]; out+=alphabet[(v>>12)&63];
    out+=i+1<n?alphabet[(v>>6)&63]:'='; out+=i+2<n?alphabet[v&63]:'=';
  }
  return out;
}
inline std::string base64(const std::string& s) { return base64(reinterpret_cast<const uint8_t*>(s.data()),s.size()); }
inline bool safeField(const std::string& s) { return !s.empty() && s.find_first_of("\r\n") == std::string::npos; }
inline bool writeText(MailIO& io,const std::string& s) { return io.write(s.data(),s.size()); }
inline bool writeEncoded(MailIO& io,const uint8_t* data,size_t n) {
  auto encoded=base64(data,n);
  std::string lines; lines.reserve(encoded.size()+encoded.size()/38+4);
  for(size_t i=0;i<encoded.size();i+=76) { lines+=encoded.substr(i,76); lines+="\r\n"; }
  return writeText(io,lines);
}
inline std::string encodedSubject(const std::string& text) {
  std::string out;
  for(size_t p=0;p<text.size();) {
    size_t end=std::min(p+size_t(42),text.size());
    while(end<text.size() && (uint8_t(text[end])&0xc0)==0x80) --end;
    if(end==p) return "invalid subject";
    if(p) out+="\r\n ";
    out+="=?UTF-8?B?"+base64(text.substr(p,end-p))+"?=";
    p=end;
  }
  return out;
}
inline Delivery beforeData(int code) { return code>=500?Delivery::PermanentFailure:Delivery::Retry; }

inline Delivery smtpSend(MailIO& io,Attachment& attachment,const Envelope& e) {
  if(!safeField(e.from)||!safeField(e.to)||!safeField(e.eventId)||!safeField(e.date)||
     !safeField(e.user)||e.password.empty()) return Delivery::PermanentFailure;
  int code=io.reply(300000);
  if(code!=220) return beforeData(code);
  auto command=[&](const std::string& text,int expected) {
    if(!writeText(io,text+"\r\n")) {code=-1; return false;}
    code=io.reply(300000); return code==expected;
  };
  if(!command("EHLO device.sound-alert.invalid",250) ||
     !command("AUTH LOGIN",334) || !command(base64(e.user),334) ||
     !command(base64(e.password),235) || !command("MAIL FROM:<"+e.from+">",250))
    return beforeData(code);
  if(!writeText(io,"RCPT TO:<"+e.to+">\r\n")) return Delivery::Retry;
  code=io.reply(300000);
  if(code!=250 && code!=251) return beforeData(code);
  if(!command("DATA",354)) return beforeData(code);
  const std::string boundary="soundalert_"+e.eventId;
  const std::string headers="Date: "+e.date+"\r\nFrom: <"+e.from+">\r\nTo: <"+e.to+
    ">\r\nSubject: "+encodedSubject(e.subject)+"\r\nMessage-ID: <"+e.eventId+
    "@device.sound-alert.invalid>\r\nX-SoundAlert-Event: "+e.eventId+
    "\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\""+boundary+"\"\r\n\r\n--"+
    boundary+"\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
  if(!writeText(io,headers)||!writeEncoded(io,reinterpret_cast<const uint8_t*>(e.body.data()),e.body.size()))
    return Delivery::Retry; // No DATA terminator has been sent; disconnect aborts submission.
  if(!writeText(io,"\r\n--"+boundary+"\r\nContent-Type: audio/wav; name=\""+e.eventId+
    ".wav\"\r\nContent-Disposition: attachment; filename=\""+e.eventId+
    ".wav\"\r\nContent-Transfer-Encoding: base64\r\n\r\n")) return Delivery::Retry;
  uint8_t chunk[57*32];
  size_t remaining=attachment.size();
  while(remaining) {
    const size_t wanted=std::min(remaining,sizeof(chunk));
    size_t total=0;
    while(total<wanted) {
      size_t n=attachment.read(chunk+total,wanted-total);
      if(!n || n>wanted-total) return Delivery::Retry;
      total+=n;
    }
    if(!writeEncoded(io,chunk,total)) return Delivery::Retry;
    remaining-=total;
  }
  // This exact write is the point where an uncertain commit becomes possible.
  if(!writeText(io,"\r\n--"+boundary+"--\r\n.\r\n")) return Delivery::Uncertain;
  auto result=afterData(io.reply(600000));
  if(result==Delivery::Accepted) writeText(io,"QUIT\r\n");
  return result;
}
}
