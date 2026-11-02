#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include "smtp_session.h"
using namespace soundalert;
struct FakeIO:MailIO {
  std::vector<int> replies={220,250,334,334,235,250,250,354,250};
  size_t position=0;std::string written;bool dropCommit=false;
  bool write(const char* s,size_t n)override{
    std::string part(s,n);written+=part;
    return !(dropCommit&&part.find("--\r\n.\r\n")!=std::string::npos);
  }
  int reply(uint32_t)override{return position<replies.size()?replies[position++]:-1;}
};
struct MemoryFile:Attachment {
  std::vector<uint8_t> data;size_t position=0,shortRead=13;
  size_t size()const override{return data.size();}
  size_t read(uint8_t* out,size_t n)override{
    n=std::min(n,std::min(shortRead,data.size()-position));
    for(size_t i=0;i<n;++i)out[i]=data[position++];return n;
  }
};
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
int main(int argc,char** argv){
  if(argc!=3)return 2;
  std::ifstream in(argv[1],std::ios::binary);
  if(!in)return 2;
  MemoryFile file;file.data=std::vector<uint8_t>(std::istreambuf_iterator<char>(in),{});
  Envelope e;e.from="sender@example.invalid";e.to="owner@example.invalid";e.user=e.from;e.password="test-only-not-a-real-password";
  e.eventId="ABC123-1234567812345678-000007";e.subject="声音提醒 "+e.eventId;
  e.body="一次新的现场检测。\r\n事件编号："+e.eventId+"\r\n附件为当次录音。\r\n";
  e.date="Tue, 20 Oct 2026 06:32:00 +0000";
  try{
    FakeIO good;
    require(smtpSend(good,file,e)==Delivery::Accepted,"normal mail not accepted");
    size_t begin=good.written.find("Date: "),end=good.written.find("\r\n.\r\n",begin);
    require(begin!=std::string::npos&&end!=std::string::npos,"MIME not terminated");
    std::ofstream out(argv[2],std::ios::binary);out<<good.written.substr(begin,end-begin)+"\r\n";out.close();
    FakeIO rcpt;rcpt.replies[6]=550;file.position=0;
    require(smtpSend(rcpt,file,e)==Delivery::PermanentFailure,"recipient rejection");
    FakeIO auth;auth.replies[4]=535;file.position=0;
    require(smtpSend(auth,file,e)==Delivery::PermanentFailure,"authentication rejection");
    FakeIO transient;transient.replies[8]=451;file.position=0;
    require(smtpSend(transient,file,e)==Delivery::Retry,"explicit post-DATA rejection");
    FakeIO timeout;timeout.replies[8]=-1;file.position=0;
    require(smtpSend(timeout,file,e)==Delivery::Uncertain,"ambiguous commit must not auto-retry");
    FakeIO disconnected;disconnected.dropCommit=true;file.position=0;
    require(smtpSend(disconnected,file,e)==Delivery::Uncertain,"lost terminator write");
    FakeIO greeting;greeting.replies[0]=-1;file.position=0;
    require(smtpSend(greeting,file,e)==Delivery::Retry,"early connection loss");
    FakeIO injection;auto bad=e;bad.to="owner@example.invalid\r\nBcc: other@example.invalid";file.position=0;
    require(smtpSend(injection,file,bad)==Delivery::PermanentFailure && injection.written.empty(),"header injection not rejected");
    require(base64(std::string("f"))=="Zg=="&&base64(std::string("fo"))=="Zm8="&&base64(std::string("foo"))=="Zm9v","base64 padding");
    std::puts("PASS: 9 SMTP/MIME cases; local fake transport only, no network mail sent.");
  }catch(const std::exception& ex){std::fprintf(stderr,"FAIL: %s\n",ex.what());return 1;}
}
