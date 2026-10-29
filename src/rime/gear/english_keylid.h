//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 蓬萊：中英混輸的按鍵層語言辨識（translator/english_keylid）
//
#ifndef RIME_ENGLISH_KEYLID_H_
#define RIME_ENGLISH_KEYLID_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rime {

// 按鍵層語言辨識表（tools/gen_keylid.py 產生的 <name>.tbl，格式寫在產生器檔頭），唯讀 mmap，不另外配記憶體。
// 同一串按鍵比兩個模型：中文注音鍵 5-gram（混打＋句尾語氣詞）、英文字母 5-gram，都是 Witten-Bell 內插。
// 兩狀態分段 Viterbi：每一鍵是中文或英文（只限 a–z），每段英文多扣一次切換分數、至少 min_length 個字母，
// 找出「強制這段是英文」的最佳切法跟最佳切法差 margin 以內的英文段，當中英混輸的切點候選。
// 跟離線的參考實作 tools/lm/keylid_vit.py 同一套算法；這裡用前向、後向各一次 Viterbi（有 beam）算出每一段的強制分數，
// 不用每一段各跑一次（tools/keylid_check.cc 對照）。只用標準函式庫和 mmap，可以單獨編。
class EnglishKeylid {
 public:
  // 打不開、格式不對時回傳 nullptr（error 寫原因）
  static std::shared_ptr<EnglishKeylid> Open(const std::string& file,
                                             std::string* error);
  ~EnglishKeylid();
  EnglishKeylid(const EnglishKeylid&) = delete;
  EnglishKeylid& operator=(const EnglishKeylid&) = delete;

  struct Params {
    double switch_penalty = 4;  // 每段英文扣的分數（keylid_vit.py 的 S = −4）
    int min_length = 2;         // 英文段最短幾個字母（L）
    int max_length = 24;        // 英文段最長幾個字母
    double margin = 8;          // 跟最佳切法差多少以內才當候選（M）
    int beam = 64;              // 每個位置留幾個中文前文（狀態）
    double beam_width = 24;     // 比同一個位置最好的狀態差這麼多以上就不留
  };

  // 一個英文段候選：[start, end)（位元組位置，一鍵一個位元組）
  struct Span {
    size_t start = 0;
    size_t end = 0;
    double gap = 0;  // 強制這段英文的最佳切法 − 最佳切法（≤ 0）
    size_t length() const { return end - start; }
  };

  // 一串按鍵的格子：建構時跑完前向、後向
  class Lattice {
   public:
    // 跟最佳切法差 margin 以內的英文段，照 gap 由大到小（同分照位置）
    const std::vector<Span>& spans() const { return spans_; }
    double best() const { return best_; }
    // 邊際 LLR：強制這段英文的最佳切法 − 強制這段中文的最佳切法（其他段自由）；後者在 beam 裡找不到時回傳 +∞
    double Llr(const Span& span) const;
    // 強制這段英文的最佳切法裡所有的英文段（含這一段），照位置
    std::vector<std::pair<size_t, size_t>> Path(const Span& span) const;

   private:
    friend class EnglishKeylid;
    struct State {
      uint32_t h;        // 中文前文：最後 4 個中文鍵（開頭補 '^'）
      double score;      // 前向：[0, pos) 的最佳分數
      double next;       // 這一鍵當中文的 log 機率（pos == n 時是結尾 '$'）
      double back;       // 後向：[pos, n) 的最佳分數
      int32_t from;      // 前向從哪裡來：中文鍵是 pos − 1；英文段是段的開頭（−1：起點）
      int32_t from_idx;  // 前一個狀態在 states_[from] 的索引
      int32_t to;        // 後向往哪裡去：中文鍵是 pos + 1；英文段是段的結尾（−1：結尾或走不通）
      int32_t to_idx;
      bool from_en;      // 從英文段來
      bool to_en;        // 往英文段去
    };
    const EnglishKeylid* model_ = nullptr;
    Params params_;
    std::string input_;
    std::vector<uint8_t> ids_;
    std::vector<size_t> run_end_;  // 字母：同一串連續字母的結尾；不是字母：0
    size_t width_ = 1;             // max_length + 1
    std::vector<double> en_;       // en_[a * width_ + len]：[a, a + len) 當一段英文（含 '$'），不能時 −∞
    std::vector<std::vector<State>> states_;
    std::vector<double> best_forward_;
    std::vector<double> best_backward_;
    std::vector<Span> spans_;
    double best_ = 0;

    double En(size_t a, size_t len) const;
    int Find(size_t pos, uint32_t h) const;
    double Forced(const Span& span, int* idx) const;
  };

  std::unique_ptr<Lattice> Analyze(const std::string& input,
                                   const Params& params) const;
  size_t file_size() const { return size_; }

 private:
  struct Model {
    const uint32_t* gram_key = nullptr;
    const float* gram_logp = nullptr;
    size_t grams = 0;
    const uint32_t* ctx_key = nullptr;
    const float* ctx_logbo = nullptr;
    size_t ctxs = 0;
    float log_v = 0;
    float bo_empty = 0;
  };
  EnglishKeylid() = default;
  // h：前面 4 個符號打包（每個 6 位元），x：下一個符號
  double Lp(const Model& m, uint32_t h, uint32_t x) const;

  void* base_ = nullptr;
  size_t size_ = 0;
  bool mapped_ = false;
  std::string owned_;  // 沒有 mmap 的平台：整個檔讀進來
  uint8_t id_of_[256] = {};
  uint8_t begin_ = 0, end_ = 0, unknown_ = 63;
  Model zh_, en_;
};

}  // namespace rime

#endif  // RIME_ENGLISH_KEYLID_H_
