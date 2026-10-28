//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 蓬萊：英文單字表（translator/english_lexicon）
//
#ifndef RIME_ENGLISH_LEXICON_H_
#define RIME_ENGLISH_LEXICON_H_

#include <rime/common.h>
#include <rime/gear/english_judge.h>

namespace rime {

// 一行一個「單字<TAB>Zipf」（Zipf = log10(每十億詞出現次數)，wordfreq 的寫法），# 開頭是註解。
// 只收全是小寫 a–z、長度 >= min_length、Zipf >= min_zipf 的詞。
class EnglishLexicon {
 public:
  // 先找使用者資料夾的 <name>.txt，再找共用資料夾；找不到或沒有詞時回傳 nullptr。
  // 同一個檔案（路徑、修改時間、門檻都相同）只讀一次，所有方案共用。
  static an<EnglishLexicon> Require(const string& name,
                                    int min_length,
                                    double min_zipf);

  // 判斷表 <name>.tbl（tools/gen_judge.py 產生）：先找使用者資料夾，再找共用資料夾；
  // 找不到或格式不對時回傳 nullptr。同一個檔案（路徑、修改時間相同）只開一次，所有方案共用。
  static an<EnglishJudge> RequireJudge(const string& name);

  bool Find(const string& word, double* zipf) const;
  size_t size() const { return words_.size(); }
  size_t max_length() const { return max_length_; }

 private:
  hash_map<string, double> words_;
  size_t max_length_ = 0;
};

}  // namespace rime

#endif  // RIME_ENGLISH_LEXICON_H_
