//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 蓬萊：中英混輸的判斷（translator/english_judge）
//
#ifndef RIME_ENGLISH_JUDGE_H_
#define RIME_ENGLISH_JUDGE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rime {

// 判斷表（tools/gen_judge.py 產生的 <name>.bin，格式寫在產生器檔頭），唯讀 mmap，不另外配記憶體。
// 句子分數 = 中文每個字的 log 機率（字三元，內插 Kneser-Ney）＋ 每段英文的字母 log 機率（字母三元）。
// 英文段（連續的 A–Z、a–z）和其他非漢字都是中文的斷點；漢字先照表裡的 TWVariants 換成臺灣字形再查。
// 跟離線的參考實作 tools/lm/judge3.py 算出來的分數相同（tools/gen_judge.py --check 驗表、
// tools/judge_check.cc 驗這支程式）。只用標準函式庫和 mmap，可以單獨編。
class EnglishJudge {
 public:
  // 打不開、格式不對時回傳 nullptr（error 寫原因）
  static std::shared_ptr<EnglishJudge> Open(const std::string& file,
                                            std::string* error);
  ~EnglishJudge();
  EnglishJudge(const EnglishJudge&) = delete;
  EnglishJudge& operator=(const EnglishJudge&) = delete;

  double Score(const std::string& text) const;
  // 一段英文（不分大小寫）的字母 log 機率
  double EnglishLogp(const std::string& word) const;
  // 漢字照表裡的 TWVariants 單字對照換成臺灣字形（simplifier@zh_hant_tw 用的也是這份對照），其他字元不動
  std::string TaiwanVariants(const std::string& text) const;
  size_t file_size() const { return size_; }

 private:
  EnglishJudge() = default;
  double ChineseLogp(uint32_t a, uint32_t b, uint32_t c) const;
  uint32_t Normalize(uint32_t cp) const;

  void* base_ = nullptr;
  size_t size_ = 0;
  bool mapped_ = false;
  std::string owned_;  // 沒有 mmap 的平台：整個檔讀進來
  double d_ = 0.75;
  double nb_ = 1;
  const uint32_t* uni_key_ = nullptr;
  const uint32_t* uni_cont_ = nullptr;
  const uint32_t* b_key_ = nullptr;
  const uint32_t* b_cont_ = nullptr;
  const uint32_t* b_off_ = nullptr;
  const uint16_t* bc_c_ = nullptr;
  const uint32_t* bc_cont_ = nullptr;
  const uint32_t* ab_key_ = nullptr;
  const uint32_t* ab_cnt_ = nullptr;
  const uint32_t* ab_off_ = nullptr;
  const uint16_t* tri_c_ = nullptr;
  const uint32_t* tri_cnt_ = nullptr;
  const uint32_t* map_ = nullptr;  // (from, to) 交錯
  const double* en_ = nullptr;     // [28][28][28]
  size_t n_uni_ = 0, n_b_ = 0, n_ab_ = 0, n_map_ = 0;
};

}  // namespace rime

#endif  // RIME_ENGLISH_JUDGE_H_
