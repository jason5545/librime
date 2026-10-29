//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-15 GONG Chen <chen.sst@gmail.com>
//
#include <rime/common.h>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/gear/abc_segmentor.h>

static const char kRimeAlphabet[] = "zyxwvutsrqponmlkjihgfedcba";

namespace rime {

AbcSegmentor::AbcSegmentor(const Ticket& ticket)
    : Segmentor(ticket), alphabet_(kRimeAlphabet) {
  if (!ticket.schema)
    return;
  if (Config* config = ticket.schema->config()) {
    config->GetString("speller/alphabet", &alphabet_);
    config->GetString("speller/delimiter", &delimiter_);
    config->GetString("speller/initials", &initials_);
    config->GetString("speller/finals", &finals_);
    // 連著的聲調鍵只有 speller 收得進來（要有數字候選），這裡跟著把它算進同一段
    config->GetBool("speller/digit_tones", &digit_tones_);
    config->GetBool("speller/digit_lead_tones", &digit_lead_tones_);
    if (auto extra_tags = config->GetList("abc_segmentor/extra_tags")) {
      for (size_t i = 0; i < extra_tags->size(); ++i) {
        if (auto value = As<ConfigValue>(extra_tags->GetAt(i))) {
          extra_tags_.insert(value->str());
        }
      }
    }
  }
  if (initials_.empty()) {
    initials_ = alphabet_;
  }
}

// 蓬萊（speller/digit_lead_tones）：開頭的聲調鍵單獨成一段 raw 時，翻譯器出得了數字候選就在那段加這個標籤；
// 收成一段的數字也帶著它
static const char kDigitLeadTag[] = "digit_lead";

static inline bool is_digit_key(char ch) {
  return ch >= '0' && ch <= '9';
}

// 整段輸入是「聲調鍵開頭、後面都是不是聲調鍵的數字鍵」：3ˇ0、4ˋ00、6ˊ912、3ˇ4ˋ0。
// 後面又出現聲調鍵（3193 是 3百、304 是 3案）就是打了完整的音節，當中文
bool AbcSegmentor::IsDigitLead(const string& input) const {
  size_t k = 0;
  while (k < input.length() && is_digit_key(input[k]) &&
         finals_.find(input[k]) != string::npos)
    ++k;
  if (k == 0 || k == input.length())
    return false;
  for (; k < input.length(); ++k) {
    if (!is_digit_key(input[k]) || finals_.find(input[k]) != string::npos)
      return false;
  }
  return true;
}

// 開頭是聲調鍵的數字收成一段，數字候選由翻譯器給。只標 digit_lead、不標 abc：其他翻譯器（custom_phrase…）不查這段，
// 碼剛好是 30 的自訂詞不會排到數字前面。翻譯器出不出得了數字（中英混輸關掉、沒有判斷表），
// 分段器不知道：開頭的聲調鍵單獨成一段 raw 時，翻譯器出得了就在那段加標籤，看過一次就記著
// （同一組元件，換方案、重新部署才會重建）。還沒看過之前照舊。
// 收過之後後面接了別的鍵（30s、303）就拆回原本的分法；拆開過又 BackSpace 回來時再收
bool AbcSegmentor::JoinDigitLead(Segmentation* segmentation) {
  if (!segmentation->empty() &&
      segmentation->front().HasTag(kDigitLeadTag))
    digit_lead_ready_ = true;
  if (!digit_lead_ready_)
    return false;
  const string& input = segmentation->input();
  const bool lead = IsDigitLead(input);
  Segment joined(0, input.length());
  joined.tags.insert(kDigitLeadTag);
  if (segmentation->GetCurrentStartPosition() == 0) {
    if (lead)
      return segmentation->AddSegment(joined);
    // 收過的那段比現在的輸入短，AddSegment 換不掉，自己拿掉；開頭的聲調鍵交給 fallback_segmentor 照舊成一段 raw
    if (!segmentation->empty() &&
        segmentation->back().HasTag(kDigitLeadTag) &&
        !segmentation->back().HasTag("raw"))
      segmentation->pop_back();
    return false;
  }
  // 開頭的聲調鍵單獨成一段 raw、後面一段：都還沒選字
  if (!lead || segmentation->size() != 2)
    return false;
  const Segment& first = segmentation->front();
  if (first.start != 0 || first.end != segmentation->back().start ||
      first.status >= Segment::kSelected || !first.HasTag("raw"))
    return false;
  segmentation->clear();
  return segmentation->AddSegment(joined);
}

bool AbcSegmentor::Proceed(Segmentation* segmentation) {
  const string& input(segmentation->input());
  DLOG(INFO) << "abc_segmentor: " << input;
  if (digit_lead_tones_ && JoinDigitLead(segmentation))
    return true;
  size_t j = segmentation->GetCurrentStartPosition();
  size_t k = j;
  bool expecting_an_initial = true;
  // 蓬萊（speller/digit_tones）：從頭到這裡都是數字鍵時，連著的聲調鍵也算進這一段（0912345678）；
  // 收過的話這一段到第一個不是數字的鍵為止，後面的中文另起一段
  bool digits_so_far = true;
  bool digit_run = false;
  for (; k < input.length(); ++k) {
    bool is_digit = input[k] >= '0' && input[k] <= '9';
    if (digit_run && !is_digit)
      break;
    bool is_letter = alphabet_.find(input[k]) != string::npos;
    bool is_delimiter = (k != 0) && (delimiter_.find(input[k]) != string::npos);
    if (!is_letter && !is_delimiter)
      break;
    bool is_initial = initials_.find(input[k]) != string::npos;
    bool is_final = finals_.find(input[k]) != string::npos;
    if (expecting_an_initial && !is_initial && !is_delimiter) {
      if (!(digit_tones_ && j == 0 && k > 0 && digits_so_far && is_digit))
        break;  // not a valid spelling.
      digit_run = true;
    }
    digits_so_far = digits_so_far && is_digit;
    // for the next character.
    expecting_an_initial = is_final || is_delimiter;
  }
  DLOG(INFO) << "[" << j << ", " << k << ")";
  if (j < k) {
    Segment segment(j, k);
    segment.tags.insert("abc");
    for (const string& tag : extra_tags_) {
      segment.tags.insert(tag);
    }
    segmentation->AddSegment(segment);
  }
  // continue this round
  return true;
}

}  // namespace rime
