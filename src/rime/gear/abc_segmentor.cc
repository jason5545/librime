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

bool AbcSegmentor::Proceed(Segmentation* segmentation) {
  const string& input(segmentation->input());
  DLOG(INFO) << "abc_segmentor: " << input;
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
