//
// Copyright RIME Developers
// Distributed under the BSD License
//
// simplistic sentence-making
//
// 2011-10-06 GONG Chen <chen.sst@gmail.com>
//

#ifndef RIME_POET_H_
#define RIME_POET_H_

#include <rime/common.h>
#include <rime/translation.h>
#include <rime/gear/translator_commons.h>
#include <rime/gear/contextual_translation.h>

namespace rime {

using WordGraph = map<int, map<int, DictEntryList>>;

class Grammar;
class Language;
struct Line;

// 蓬萊：MakeSentences 的 beam 狀態（每個位置的候選句），留著給下一個鍵沿用
struct PoetStates;
an<PoetStates> NewPoetStates();

class Poet {
 public:
  // Line "less", used to compare composed line of the same input range.
  using Compare = function<bool(const Line&, const Line&)>;

  static bool CompareWeight(const Line& one, const Line& other);
  static bool LeftAssociateCompare(const Line& one, const Line& other);

  Poet(const Language* language,
       Config* config,
       Compare compare = CompareWeight);
  ~Poet();

  an<Sentence> MakeSentence(const WordGraph& graph,
                            size_t total_length,
                            const string& preceding_text);
  deque<an<Sentence>> MakeSentences(const WordGraph& graph,
                                    size_t total_length,
                                    const string& preceding_text,
                                    size_t count,
                                    double cutoff_threshold);
  // 蓬萊：同上，beam 狀態放在 states。reuse_upto ≥ 0 時，位置 ≤ reuse_upto 的狀態沿用 states 裡的
  // （呼叫的人保證：上一次用同一份前文、同一個 count 算出來，起點 < reuse_upto 的詞條跟這次是同一批物件，
  // reuse_upto 也不是上次和這次的結尾），只算終點在它後面的；< 0 時從頭算。結果跟從頭算逐位元組相同
  deque<an<Sentence>> MakeSentences(const WordGraph& graph,
                                    size_t total_length,
                                    const string& preceding_text,
                                    size_t count,
                                    double cutoff_threshold,
                                    PoetStates* states,
                                    int reuse_upto);

  // 蓬萊：一個詞接在 context 後面的分數，跟組句時同一套（詞條權重加語言模型）。整句只有一個詞時用
  double Evaluate(const string& context, const DictEntry& entry, bool is_rear) const;

  template <class TranslatorT>
  an<Translation> ContextualWeighted(an<Translation> translation,
                                     const string& input,
                                     size_t start,
                                     TranslatorT* translator) {
    if (!translator->contextual_suggestions() || !grammar_) {
      return translation;
    }
    auto preceding_text = translator->GetPrecedingText(start);
    if (preceding_text.empty()) {
      return translation;
    }
    return New<ContextualTranslation>(translation, input, preceding_text,
                                      grammar_.get());
  }

 private:
  template <class Strategy>
  an<Sentence> MakeSentenceWithStrategy(const WordGraph& graph,
                                        size_t total_length,
                                        const string& preceding_text);

  const Language* language_;
  the<Grammar> grammar_;
  Compare compare_;
};

}  // namespace rime

#endif  // RIME_POET_H_
