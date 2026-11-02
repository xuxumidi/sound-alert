#pragma once
namespace soundalert {
enum class Delivery { Accepted, Retry, PermanentFailure, Uncertain };
// A disconnect after the DATA terminator is deliberately not auto-retried.
inline Delivery afterData(int smtpCode) {
  if(smtpCode==250) return Delivery::Accepted;
  if(smtpCode>=400 && smtpCode<500) return Delivery::Retry;
  if(smtpCode>=500) return Delivery::PermanentFailure;
  return Delivery::Uncertain;
}
}
