#include <algorithm>
#include <iterator>
#include <rime/gear/contextual_translation.h>
#include <rime/gear/grammar.h>
#include <rime/gear/translator_commons.h>

namespace rime {

const int kContextualSearchLimit = 32;

bool ContextualTranslation::Replenish() {
  vector<of<Phrase>> queue;
  size_t end_pos = 0;
  std::string last_type;
  while (!translation_->exhausted() &&
         cache_.size() + queue.size() < kContextualSearchLimit) {
    auto cand = translation_->Peek();
    DLOG(INFO) << cand->text() << " cache/queue: " << cache_.size() << "/"
               << queue.size();
    if (cand->type() == "phrase" || cand->type() == "user_phrase" ||
        cand->type() == "table" || cand->type() == "user_table" ||
        cand->type() == "completion") {
      if (end_pos != cand->end() || last_type != cand->type()) {
        end_pos = cand->end();
        last_type = cand->type();
        AppendToCache(queue);
      }
      queue.push_back(Evaluate(As<Phrase>(cand)));
    } else {
      AppendToCache(queue);
      cache_.push_back(cand);
    }
    if (!translation_->Next()) {
      break;
    }
  }
  AppendToCache(queue);
  return !cache_.empty();
}

an<Phrase> ContextualTranslation::Evaluate(an<Phrase> phrase) {
  bool is_rear = phrase->end() == input_.length();
  double weight = Grammar::Evaluate(preceding_text_, phrase->text(),
                                    phrase->weight(), is_rear, grammar_);
  phrase->set_weight(weight);
  DLOG(INFO) << "contextual suggestion: " << phrase->text()
             << " weight: " << phrase->weight();
  return phrase;
}

static bool compare_by_weight_desc(const an<Phrase>& a, const an<Phrase>& b) {
  return a->weight() > b->weight();
}

void ContextualTranslation::AppendToCache(vector<of<Phrase>>& queue) {
  if (queue.empty())
    return;
  DLOG(INFO) << "appending to cache " << queue.size() << " candidates.";
  // 蓬萊：按鍵模型。最前面那一組（前面沒有整句）原本第一個是照打的詞時，重排之後第一個還是照打的：
  // 容錯的詞可以排第二以後，不搶第一（ScriptTranslation 已經把照打的排到第一個）
  const bool any_correction =
      std::any_of(queue.begin(), queue.end(),
                  [](const an<Phrase>& p) { return p->correction(); });
  if (!any_correction) {
    std::sort(queue.begin(), queue.end(), compare_by_weight_desc);
    std::copy(queue.begin(), queue.end(), std::back_inserter(cache_));
    queue.clear();
    return;
  }
  const bool head_exact = cache_.empty() && !queue.front()->correction();
  std::stable_sort(queue.begin(), queue.end(), compare_by_weight_desc);
  if (head_exact && queue.front()->correction()) {
    auto it = std::find_if(queue.begin(), queue.end(),
                           [](const an<Phrase>& p) { return !p->correction(); });
    if (it != queue.end())
      std::rotate(queue.begin(), it, it + 1);
  }
  std::copy(queue.begin(), queue.end(), std::back_inserter(cache_));
  queue.clear();
}

}  // namespace rime
