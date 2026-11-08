//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Created by nameoverflow on 2018/11/14.
//

#ifndef RIME_CORRECTOR_H
#define RIME_CORRECTOR_H

#include <cmath>
#include <limits>
#include <rime/common.h>
#include <rime/component.h>
#include <rime/algo/algebra.h>
#include <rime/algo/spelling.h>
#include <rime/dict/prism.h>
#include <rime/dict/vocabulary.h>

namespace rime {
struct Ticket;

class SymDeleteCollector {
 public:
  explicit SymDeleteCollector(const Syllabary& syllabary)
      : syllabary_(syllabary) {}

  Script Collect(size_t edit_distance);

 private:
  const Syllabary& syllabary_;
};

namespace corrector {
using Distance = size_t;
struct Correction {
  size_t distance;
  SyllableId syllable;
  size_t length;
  // 蓬萊：按鍵模型（PenglaiKeyCorrector）算的 log 機率，錯幾次就加幾次；scored 為 false 時照原本固定扣分
  double score = 0.0;
  bool scored = false;
  bool omission = false;  // 補了漏打的鍵（只給完整音節用，不給簡拼）
  // 蓬萊：用到哪幾種錯（PenglaiKeyCorrector::kOp*），簡拼只收設定允許的
  unsigned ops = 0;
  // 蓬萊：這個拼法最後一個鍵是照打的聲調鍵（打的人把這個音節打完了）
  bool closed = false;
};
// 蓬萊：原本用拼法編號當 key，同一個拼法只能有一種長度；按鍵模型的多按一下（ㄐㄐ 當一個 ㄐ）會讓同一個拼法
// 吃不同長度的輸入，改用（拼法, 長度）當 key。Correction::syllable 照舊是拼法編號
class Corrections : public hash_map<uint64_t, Correction> {
 public:
  static uint64_t Key(SyllableId syllable, size_t length) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(syllable)) << 32) |
           static_cast<uint32_t>(length);
  }
  /// Update for better correction
  /// \param syllable
  /// \param correction
  inline void Alter(SyllableId syllable, Correction correction) {
    auto key = Key(syllable, correction.length);
    auto it = find(key);
    if (it == end()) {
      (*this)[key] = correction;
    } else if (correction.scored ? correction.score > it->second.score
                                 : correction.distance < it->second.distance) {
      it->second = correction;
    }
  };
};
}  // namespace corrector

/**
 * The unify interface of correctors
 */
class Corrector : public Class<Corrector, const Ticket&> {
 public:
  virtual ~Corrector() = default;
  RIME_DLL virtual void ToleranceSearch(const Prism& prism,
                                        const string& key,
                                        corrector::Corrections* results,
                                        size_t tolerance) = 0;
  // 蓬萊：按鍵模型要知道這一段前面那個鍵（連按兩下），所以給整串輸入和這一段的起點
  virtual void ScoredSearch(const Prism& prism,
                            const string& input,
                            size_t begin,
                            corrector::Corrections* results) {
    ToleranceSearch(prism, input.substr(begin), results, 5);
  }
  // 蓬萊：容錯結果帶分數（Correction::score）、簡拼也能容錯
  virtual bool Scored() const { return false; }
  virtual bool CorrectsAbbreviations() const { return false; }
  // closed：這個拼法是照打的聲調鍵結尾（沒打聲調的完整音節在方案裡也算簡拼）
  virtual bool AbbreviationAllows(unsigned ops, bool closed) const { return false; }
  // 蓬萊：照打的那段本身就是完整音節時，容錯再加的 log 分數（照打合法時多半就是要它）
  virtual double ValidPenalty() const { return 0.0; }
  virtual bool ValidSuggest() const { return false; }
  // 蓬萊：照打合法時，這組錯（ops）的扣分、是不是只當建議。觸控給了按下去的位置時另外算（PenglaiKeyCorrector）
  virtual double ValidPenaltyFor(unsigned ops) const { return ValidPenalty(); }
  virtual bool ValidSuggestFor(unsigned ops) const { return ValidSuggest(); }
  virtual bool ClosedType() const { return false; }
  virtual bool ValidFirst() const { return false; }
  // 蓬萊：只修打了聲調的音節（例外是沒打聲調的模糊音，見 PenglaiKeyCorrector::Options::fuzzy_untoned）
  virtual bool RequiresTone() const { return false; }
  // 蓬萊：修過的詞要不要在註解寫上修成的讀音
  virtual bool ShowReading() const { return false; }
  // 蓬萊：觸控的簡拼（一個鍵的聲母照觸控換成附近的聲母），沒打聲調也收
  virtual bool IsTouchAbbreviation(unsigned ops) const { return false; }
};

// 蓬萊：按鍵模型（照 Google 注音的做法：每一鍵都是機率，跟語言模型分數一起排）。
// 方案的 translator/key_model 打開時用這個，取代 NearSearchCorrector。實體鍵盤沒有碰觸座標，
// 按錯鍵的機率照美式鍵盤上兩鍵中心的距離（二維常態分布）分給附近的鍵；另外加上 Google 沒做的
// 多按一鍵（同一鍵連按兩下另計）、漏打聲母、漏打介音、模糊音。錯幾次就加幾次 log 機率
// 蓬萊：觸控。ScriptSyllabifier 建音節圖之前設好這一段輸入每個鍵的觸控（Context::touches，offset 是這段輸入在
// 整串輸入的起點），PenglaiKeyCorrector::ScoredSearch 讀；離開時還原。同一條執行緒同時只會建一張圖
class RIME_DLL TouchScope {
 public:
  // full_style：打法偵測（translator/typing_style）判定這個人只打完整注音。這時觸控換成聲調鍵照 touch_near
  // 算打完音節、不做觸控的簡拼；其他（只打首注音、混打、還沒判定）照 touch_close、touch_abbreviations
  TouchScope(const vector<string>* touches, size_t offset, bool full_style = false);
  ~TouchScope();
  // 整串輸入第 offset + pos 個鍵的觸控；沒有就是 nullptr
  static const string* At(size_t pos);
  static bool FullStyle();

 private:
  const vector<string>* saved_touches_;
  size_t saved_offset_;
  bool saved_full_style_;
};

class RIME_DLL PenglaiKeyCorrector : public Corrector {
 public:
  struct Options {
    double substitution = 0.01;   // 按到旁邊的鍵（總和）
    double sigma = 0.6;           // 鍵距的標準差（鍵寬為 1）
    double extra = 0.002;         // 多按一個鍵
    double double_press = 0.01;   // 同一個鍵連按兩下
    double omit_initial = 0.003;  // 漏打聲母
    double omit_medial = 0.003;   // 漏打介音（ㄧㄨㄩ）
    double fuzzy = 0.01;          // 模糊音（fuzzy_pairs 裡的每一對；沒寫清單就沒有）
    vector<string> fuzzy_pairs;   // 注音兩兩一對，例如 ㄣㄥ
    int max_edits = 1;            // 一個音節最多錯幾次
    // 簡拼（只打第一個注音）收哪幾種錯：none（預設）、double（只收連按兩下）、all
    string abbreviations = "none";
    // 只修以照打的聲調鍵結尾的音節（看起來是打完整注音的地方）；只打第一個注音的輸入不碰
    bool require_tone = true;
    // 模糊音例外：沒打聲調也修（ㄈㄡ ㄓㄣ → 佛振）。只收只換了模糊音、聲母開頭又有韻母或介音、照打也是完整音節的；
    // 整串裡有單獨的聲母（只打首注音的打法）就不修
    bool fuzzy_untoned = true;
    // 修過的詞在選字窗的註解寫上修成的讀音（佛振 ㄈㄛˊ ㄓㄣˋ）
    bool show_reading = true;
    // 照打的那段本身就是完整音節時，容錯的機率再乘這個（換成另一個合法音節）
    double valid = 0.0001;
    // 照打合法的容錯不進整句，只當整段一個詞的建議（最多排第二）
    bool valid_suggest = true;
    // 修好的完整音節當成照打的完整音節算類型（會剪掉同一段的簡拼讀法）
    bool closed_type = false;
    // 照打合法時的建議詞（整段一個詞）分數贏過最好的整句時可以排第一；預設不行，最多排第二
    bool valid_first = false;
    // 觸控（手機前端傳了按下去的位置時取代上面的鍵距）：本來要按 k、按到 typed 的機率
    // = touch_prior × exp(−(d²(k) − d²(typed)) / (2 touch_sigma²))，d 是按下去的位置到鍵中心的距離（鍵寬為 1）。
    // 0.25（0069，原本 0.35）：Gboard 的觸控空間模型每個鍵是 mm 單位的二維常態分布，σx² = 0.0075·(1.5×鍵寬)² + 1.68、
    // σy² = 0.0108·(1.5×鍵高)² + 1.33；POCO F8 Ultra 上蓬萊注音鍵盤的鍵 7.27×9.09 mm 代進去是 σx 0.22、σy 0.25 鍵寬。
    // 0.35 寬了一倍半，按得準的時候照打合法的也常被換掉（空學習紀錄、打對的長句 622 句改壞 63 句）。
    // 取 0.25 不取 0.22：手指不穩（雜訊 0.35 鍵寬）時 0.22 救回的少，Jason 實際按多偏還沒量（前端 TouchStats 在量）
    double touch_sigma = 0.25;
    double touch_prior = 1.0;
    // 照打本身是完整音節、但按下去的位置離別的鍵也近時：容錯的機率再乘這個（取代 valid），而且進整句、不只當建議。
    // 按下去的位置就是證據（實體鍵盤沒有，所以照打合法時只當建議）。0.1（0069，原本 1，不扣）：照打合法時還是要扣一點，
    // 不然「上個」「開個」這種讀音比重 0% 的照打（個 ㄍㄜ˙）一按偏就被換成「上格」「開格」
    double touch_valid = 0.1;
    // 按下去的位置要多靠近另一鍵才算上一條的證據：d²(k) − d²(typed) 不超過這個（鍵寬為 1；兩鍵中心連線上
    // 離打出來的鍵中心 x 時是 1 − 2x，0.95 是偏了 0.025 鍵寬）。按在鍵中間的照實體鍵盤的規則（valid、只當建議）
    double touch_near = 0.95;
    // 觸控換成聲調鍵時，要多靠近那個聲調鍵才算「打完這個音節」（同一個量 d²(k) − d²(typed)；0.4 是離兩鍵交界
    // 0.2 鍵寬以內）。比 touch_near 嚴：ㄉ、ㄓ、ㄚ 緊鄰聲調鍵，只打首注音的「可以的」（ㄎ ㄧ ㄉ）按在 ㄉ 偏右，
    // 照 touch_near 就會被讀成「可以」（ㄎ ㄧˇ），最後的「的」不見（11/7 Jason「用起來怪怪的」調查）
    double touch_close = 0.4;
    // 觸控的簡拼：只打第一個注音的聲母（一個鍵）也照按下去的位置換成附近的聲母（Gboard 的單獨聲母也有觸控的
    // 鄰鍵）。只收聲母換聲母、整個拼法就是那一鍵；照打的聲母本身是合法的簡拼，跟照打合法一樣扣 touch_valid。
    // 0071 起也管沒打聲調的完整音節（有聲母、有介音或韻母，只照觸控換鍵、按得夠靠近 touch_close）：require_tone 開著時原本不修
    bool touch_abbreviations = true;
    // 一個音節最多錯幾次，算上觸控靠近另一鍵的換鍵（那種換鍵不佔 max_edits）
    int touch_max_edits = 2;
  };
  enum : unsigned {
    kOpSubstitution = 1,
    kOpFuzzy = 2,
    kOpExtra = 4,
    kOpDouble = 8,
    kOpOmission = 16,
    kOpTouch = 32,  // 按到旁邊的鍵，照觸控按下去的位置算的（跟 kOpSubstitution 一起）
    kOpTouchClose = 64,  // 觸控換成聲調鍵、而且按得夠靠近（touch_close）：算打完音節（跟 kOpTouch 一起）
    kOpTouchSub = 128,  // 換鍵照觸控算的（不管靠不靠近；跟 kOpSubstitution 一起）
  };
  explicit PenglaiKeyCorrector(const Options& options);
  void ToleranceSearch(const Prism& prism,
                       const string& key,
                       corrector::Corrections* results,
                       size_t tolerance) override;
  void ScoredSearch(const Prism& prism,
                    const string& input,
                    size_t begin,
                    corrector::Corrections* results) override;
  bool Scored() const override { return true; }
  bool ValidSuggest() const override { return options_.valid_suggest; }
  bool ClosedType() const override { return options_.closed_type; }
  bool ValidFirst() const override { return options_.valid_first; }
  bool RequiresTone() const override { return options_.require_tone; }
  bool ShowReading() const override { return options_.show_reading; }
  bool IsTouchAbbreviation(unsigned ops) const override {
    return TouchAbbreviation(ops);
  }
  double ValidPenalty() const override {
    if (options_.valid <= 0)
      return -std::numeric_limits<double>::infinity();  // 照打合法時不修
    return options_.valid < 1 ? std::log(options_.valid) : 0.0;
  }
  double ValidPenaltyFor(unsigned ops) const override {
    if (ops & kOpTouch)
      return options_.touch_valid > 0 && options_.touch_valid < 1
                 ? std::log(options_.touch_valid)
                 : (options_.touch_valid <= 0
                        ? -std::numeric_limits<double>::infinity()
                        : 0.0);
    return ValidPenalty();
  }
  bool ValidSuggestFor(unsigned ops) const override {
    return (ops & kOpTouch) ? false : options_.valid_suggest;
  }
  bool CorrectsAbbreviations() const override {
    return options_.abbreviations != "none";
  }
  // 這組錯能不能用在簡拼上。沒打聲調、只換了模糊音的（fuzzy_untoned，ScoredSearch 只在拼法有韻母或介音時給）照收
  bool AbbreviationAllows(unsigned ops, bool closed) const override {
    if (!closed && ops == kOpFuzzy && options_.fuzzy_untoned &&
        options_.require_tone)
      return true;
    if (TouchAbbreviation(ops))
      return true;
    if (options_.abbreviations == "all")
      return !(ops & kOpOmission);
    if (options_.abbreviations == "double")
      return ops == kOpDouble;
    return false;
  }

 private:
  // 觸控的簡拼（touch_abbreviations）：只有照觸控換鍵，沒有別的錯
  bool TouchAbbreviation(unsigned ops) const {
    return options_.touch_abbreviations && !TouchScope::FullStyle() &&
           (ops & kOpTouchSub) &&
           !(ops & ~(kOpSubstitution | kOpTouch | kOpTouchClose | kOpTouchSub));
  }
  bool Untoned(unsigned ops, bool has_final) const;
  // 這一鍵有觸控時，照按下去的位置算換成附近各鍵的機率；沒有觸控（或格式不對）回 false，用鍵距表
  struct TouchAlt {
    char key;
    double cost;
    bool near;  // 按下去的位置靠近這一鍵（kOpTouch）
    bool close;  // 更靠近（touch_close）：換成聲調鍵時算打完音節（kOpTouchClose）
  };
  bool TouchSubstitutes(size_t pos, char typed, vector<TouchAlt>* out) const;
  Options options_;
  // 打出 typed 鍵、本來要按的是 intended 鍵：每個 typed 鍵的候選（intended, log 機率）
  hash_map<char, vector<pair<char, double>>> substitutes_;
  hash_map<char, vector<pair<char, double>>> fuzzy_;
  double log_extra_, log_double_, log_omit_initial_, log_omit_medial_;
};

class CorrectorComponent : public Corrector::Component {
 public:
  CorrectorComponent();
  ~CorrectorComponent() override = default;
  Corrector* Create(const Ticket& ticket) noexcept override;

 private:
  template <class... Cs>
  static Corrector* Combine(Cs... args);

  the<ResourceResolver> resolver_;

  class Unified : public Corrector {
   public:
    Unified() = default;
    RIME_DLL void ToleranceSearch(const Prism& prism,
                                  const string& key,
                                  corrector::Corrections* results,
                                  size_t tolerance) override;
    template <class... Cs>
    void Add(Cs... args) {
      contents = {args...};
    }

   private:
    vector<of<Corrector>> contents = {};
  };
};

class EditDistanceCorrector : public Corrector, public Prism {
 public:
  ~EditDistanceCorrector() override = default;
  RIME_DLL explicit EditDistanceCorrector(const path& file_path);

  RIME_DLL bool Build(const Syllabary& syllabary,
                      const Script* script = nullptr,
                      uint32_t dict_file_checksum = 0,
                      uint32_t schema_file_checksum = 0);

  RIME_DLL void ToleranceSearch(const Prism& prism,
                                const string& key,
                                corrector::Corrections* results,
                                size_t tolerance) override;
  corrector::Distance LevenshteinDistance(const std::string& s1,
                                          const std::string& s2);
  corrector::Distance RestrictedDistance(const std::string& s1,
                                         const std::string& s2,
                                         corrector::Distance threshold);
};

class RIME_DLL NearSearchCorrector : public Corrector {
 public:
  NearSearchCorrector() = default;
  ~NearSearchCorrector() override = default;
  void ToleranceSearch(const Prism& prism,
                       const string& key,
                       corrector::Corrections* results,
                       size_t tolerance) override;
};

template <class... Cs>
Corrector* CorrectorComponent::Combine(Cs... args) {
  auto u = new Unified();
  u->Add(args...);
  return u;
}

}  // namespace rime

#endif  // RIME_CORRECTOR_H
