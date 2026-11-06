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
  virtual bool AbbreviationAllows(unsigned ops) const { return false; }
  // 蓬萊：照打的那段本身就是完整音節時，容錯再加的 log 分數（照打合法時多半就是要它）
  virtual double ValidPenalty() const { return 0.0; }
  virtual bool ValidSuggest() const { return false; }
  virtual bool ClosedType() const { return false; }
  virtual bool ValidFirst() const { return false; }
};

// 蓬萊：按鍵模型（照 Google 注音的做法：每一鍵都是機率，跟語言模型分數一起排）。
// 方案的 translator/key_model 打開時用這個，取代 NearSearchCorrector。實體鍵盤沒有碰觸座標，
// 按錯鍵的機率照美式鍵盤上兩鍵中心的距離（二維常態分布）分給附近的鍵；另外加上 Google 沒做的
// 多按一鍵（同一鍵連按兩下另計）、漏打聲母、漏打介音、模糊音。錯幾次就加幾次 log 機率
class RIME_DLL PenglaiKeyCorrector : public Corrector {
 public:
  struct Options {
    double substitution = 0.01;   // 按到旁邊的鍵（總和）
    double sigma = 0.6;           // 鍵距的標準差（鍵寬為 1）
    double extra = 0.002;         // 多按一個鍵
    double double_press = 0.01;   // 同一個鍵連按兩下
    double omit_initial = 0.003;  // 漏打聲母
    double omit_medial = 0.003;   // 漏打介音（ㄧㄨㄩ）
    double fuzzy = 0.0;           // 模糊音（fuzzy_pairs 裡的每一對）
    vector<string> fuzzy_pairs;   // 注音兩兩一對，例如 ㄣㄥ
    int max_edits = 2;
    // 簡拼（只打第一個注音）收哪幾種錯：none（預設）、double（只收連按兩下）、all
    string abbreviations = "none";
    // 只修以照打的聲調鍵結尾的音節（看起來是打完整注音的地方）；只打第一個注音的輸入不碰
    bool require_tone = true;
    // 照打的那段本身就是完整音節時，容錯的機率再乘這個（換成另一個合法音節）
    double valid = 1.0;
    // 照打合法的容錯不進整句，只當整段一個詞的建議（最多排第二）
    bool valid_suggest = false;
    // 修好的完整音節當成照打的完整音節算類型（會剪掉同一段的簡拼讀法）
    bool closed_type = false;
    // 照打合法時的建議詞（整段一個詞）分數贏過最好的整句時可以排第一；預設不行，最多排第二
    bool valid_first = false;
  };
  enum : unsigned {
    kOpSubstitution = 1,
    kOpFuzzy = 2,
    kOpExtra = 4,
    kOpDouble = 8,
    kOpOmission = 16,
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
  double ValidPenalty() const override {
    if (options_.valid <= 0)
      return -std::numeric_limits<double>::infinity();  // 照打合法時不修
    return options_.valid < 1 ? std::log(options_.valid) : 0.0;
  }
  bool CorrectsAbbreviations() const override {
    return options_.abbreviations != "none";
  }
  // 這組錯能不能用在簡拼上
  bool AbbreviationAllows(unsigned ops) const {
    if (options_.abbreviations == "all")
      return !(ops & kOpOmission);
    if (options_.abbreviations == "double")
      return ops == kOpDouble;
    return false;
  }

 private:
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
