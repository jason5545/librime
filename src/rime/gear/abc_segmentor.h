//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-20 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_ABC_SEGMENTOR_H_
#define RIME_ABC_SEGMENTOR_H_

#include <rime/segmentor.h>

namespace rime {

class AbcSegmentor : public Segmentor {
 public:
  explicit AbcSegmentor(const Ticket& ticket);

  virtual bool Proceed(Segmentation* segmentation);

 protected:
  string alphabet_;
  string delimiter_;
  string initials_;
  string finals_;
  set<string> extra_tags_;
  // 蓬萊：整段數字鍵裡連著的聲調鍵也算進這一段（speller/digit_tones）
  bool digit_tones_ = false;
  // 蓬萊：開頭是聲調鍵的數字（30、6912）收成一段（speller/digit_lead_tones）
  bool JoinDigitLead(Segmentation* segmentation);
  bool IsDigitLead(const string& input) const;
  bool digit_lead_tones_ = false;
  bool digit_lead_ready_ = false;  // 看過翻譯器加的標籤：出得了數字候選
};

}  // namespace rime

#endif  // RIME_ABC_SEGMENTOR_H_
