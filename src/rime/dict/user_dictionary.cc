//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-10-30 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <boost/algorithm/string.hpp>
#include <boost/scope_exit.hpp>
#include <rime/common.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/algo/dynamics.h>
#include <cstring>
#include <rime/algo/syllabifier.h>
#include <rime/algo/strings.h>
#include <rime/dict/db.h>
#include <rime/dict/level_db.h>
#include <rime/dict/table.h>
#include <rime/dict/user_dictionary.h>
#include <rime/dict/vocabulary.h>

namespace rime {

struct DfsState {
  size_t watermark = 0;  // 蓬萊：讀過的最遠位置
  size_t depth_limit;
  size_t predict_word_from_depth;
  TickCount present_tick;
  Code code;
  vector<double> credibility;
  vector<double> quality_len;
  hash_map<int, DictEntryList> query_result;
  an<DbAccessor> accessor;
  // 蓬萊：目前這一筆，指著 accessor 讀到的那一筆（GetNextRecordRef，不複製）；讀到底時指著空字串
  const string* key = &kEmpty;
  const string* value = &kEmpty;
  static const string kEmpty;
  // 蓬萊：記憶體副本的整個陣列（SortedRecords；沒有時 nullptr，照原本一個一個音節 Jump），
  // 和已知的前綴範圍 [lo, hi)（最上層是整個陣列，DfsLookup 算過自己這一層的才放進來）
  const vector<pair<string, string>>* rows = nullptr;
  vector<pair<size_t, size_t>> ranges;
  // 蓬萊：學習紀錄的版本（LevelDb 才有；TopLowerBound 的快取靠它判斷副本有沒有換過）
  bool has_version = false;
  size_t data_version = 0;

  // 讀取位置設成「Jump 到第 index 筆、再讀一筆」之後的樣子（跟 ForwardScan 的結果相同）
  void PlaceAfter(size_t index) {
    if (index < rows->size()) {
      key = &(*rows)[index].first;
      value = &(*rows)[index].second;
      accessor->SeekRecord(index + 1);
    } else {
      key = &kEmpty;
      value = &kEmpty;
      accessor->SeekRecord(rows->size());
    }
  }

  size_t depth() const { return code.size(); }

  bool IsExactMatch(const string& prefix) {
    // 蓬萊：同 starts_with(key, prefix + '\t')，不另外配置字串
    return key->size() > prefix.size() &&
           key->compare(0, prefix.size(), prefix) == 0 &&
           (*key)[prefix.size()] == '\t';
  }
  bool IsPrefixMatch(const string& prefix) {
    return boost::starts_with(*key, prefix);
  }
  void RecruitEntry(size_t pos,
                    hash_map<string, SyllableId>* syllabary = nullptr);
  bool NextEntry() {
    if (!accessor->GetNextRecordRef(&key, &value)) {
      key = &kEmpty;
      value = &kEmpty;
      return false;  // reached the end
    }
    return true;
  }
  bool ForwardScan(const string& prefix) {
    if (!accessor->Jump(prefix)) {
      return false;
    }
    return NextEntry();
  }
  bool Backdate(const string& prefix) {
    DLOG(INFO) << "backdate; prefix: " << prefix;
    if (!accessor->Reset()) {
      LOG(WARNING) << "backdating failed for '" << prefix << "'.";
      return false;
    }
    return NextEntry();
  }
};

const string DfsState::kEmpty;

namespace {

// 蓬萊：key 從第 base 個位元組起，跟 syllable＋' ' 逐位元組（unsigned，同 std::string 的比較）比：
// -1 比較小、0 以它開頭、1 比較大（不以它開頭）
int compare_after(const string& key, size_t base, const string& syllable) {
  const size_t rest = key.size() - base;
  const size_t n = (std::min)(rest, syllable.size());
  int c = std::memcmp(key.data() + base, syllable.data(), n);
  if (c != 0)
    return c < 0 ? -1 : 1;
  if (rest <= syllable.size())
    return -1;  // key 剩下的是 syllable 的前段（或剛好是它，少了空白）
  const unsigned char next = key[base + syllable.size()];
  if (next == ' ')
    return 0;
  return next < ' ' ? -1 : 1;
}


// 蓬萊：syllable＋' ' 是否小於 other＋' '（ForwardScan 的前綴照這個順序）
bool syllable_less(const char* syllable,
                   size_t syllable_size,
                   const char* other,
                   size_t other_size) {
  const size_t n = (std::min)(syllable_size, other_size);
  int c = std::memcmp(syllable, other, n);
  if (c != 0)
    return c < 0;
  if (syllable_size == other_size)
    return false;
  // 短的那個接著是 ' '，跟長的那個的下一個位元組比
  if (syllable_size < other_size)
    return ' ' < static_cast<unsigned char>(other[n]);
  return static_cast<unsigned char>(syllable[n]) < ' ';
}

bool syllable_less(const string& syllable, const string& other) {
  return syllable_less(syllable.data(), syllable.size(), other.data(),
                       other.size());
}

// 蓬萊：[from, to) 裡第一筆 compare_after ≥ 0 的（from 之前的都比較小），從 from 往後指數搜尋。
// 找到的那一筆（< to 時）compare_after 的結果放進 *cmp（0：以「音節＋空白」開頭），不用再比一次
size_t gallop_after(const vector<pair<string, string>>& rows,
                    size_t from,
                    size_t to,
                    size_t base,
                    const string& syllable,
                    int* cmp) {
  if (from >= to)
    return from;
  const int first = compare_after(rows[from].first, base, syllable);
  if (first >= 0) {
    *cmp = first;
    return from;
  }
  size_t lo = from + 1;  // lo 之前的都比較小
  size_t step = 1;
  while (true) {
    size_t probe = lo + step - 1;
    if (probe >= to) {
      probe = to;
      break;
    }
    if (compare_after(rows[probe].first, base, syllable) >= 0)
      break;
    lo = probe + 1;
    step <<= 1;
  }
  size_t end = (std::min)(lo + step, to);  // 答案在 [lo, end] 之間（end 可能是 to）
  const size_t found =
      std::partition_point(rows.begin() + lo, rows.begin() + end,
                           [&](const pair<string, string>& row) {
                             return compare_after(row.first, base, syllable) <
                                    0;
                           }) -
      rows.begin();
  if (found < to)
    *cmp = compare_after(rows[found].first, base, syllable);
  return found;
}
}  // namespace

void DfsState::RecruitEntry(size_t pos,
                            hash_map<string, SyllableId>* syllabary) {
  string full_code;
  auto e = UserDictionary::CreateDictEntry(
      *key, *value, present_tick, credibility.back(), quality_len.back(),
      syllabary ? &full_code : nullptr);
  if (e) {
    if (syllabary) {
      vector<string> syllables =
          strings::split(full_code, " ", strings::SplitBehavior::SkipToken);
      Code numeric_code;
      for (auto s = syllables.begin(); s != syllables.end(); ++s) {
        auto found = syllabary->find(*s);
        if (found == syllabary->end()) {
          LOG(ERROR) << "failed to recruit dict entry '" << e->text
                     << "', unrecognized syllable: " << *s;
          return;
        }
        numeric_code.push_back(found->second);
      }
      e->code = numeric_code;
      e->matching_code_size = code.size();
    } else {
      e->code = code;
    }
    DLOG(INFO) << "add entry at pos " << pos;
    query_result[pos].push_back(e);
  }
}

// UserDictEntryIterator members

void UserDictEntryIterator::Add(an<DictEntry>&& entry) {
  cache_.push_back(std::move(entry));
}

void UserDictEntryIterator::SetEntries(DictEntryList&& entries) {
  cache_ = std::move(entries);
}

void UserDictEntryIterator::SortRange(size_t start, size_t count) {
  cache_.SortRange(start, count);
}

void UserDictEntryIterator::AddFilter(DictEntryFilter filter) {
  DictEntryFilterBinder::AddFilter(filter);
  // the introduced filter could invalidate the current or even all the
  // remaining entries
  while (!exhausted() && !filter_(Peek())) {
    FindNextEntry();
  }
}

an<DictEntry> UserDictEntryIterator::Peek() {
  if (exhausted()) {
    return nullptr;
  }
  return cache_[index_];
}

bool UserDictEntryIterator::FindNextEntry() {
  if (exhausted()) {
    return false;
  }
  ++index_;
  return !exhausted();
}

bool UserDictEntryIterator::Next() {
  if (!FindNextEntry()) {
    return false;
  }
  while (filter_ && !filter_(Peek())) {
    if (!FindNextEntry()) {
      return false;
    }
  }
  return true;
}

// UserDictionary members

UserDictScoring& UserDictionary::scoring() {
  static UserDictScoring s_scoring;
  return s_scoring;
}

UserDictionary::UserDictionary(const string& name, an<Db> db)
    : name_(name), db_(db) {}

UserDictionary::~UserDictionary() {
  if (loaded()) {
    CommitPendingTransaction();
  }
}

void UserDictionary::Attach(const an<Table>& table, const an<Prism>& prism) {
  table_ = table;
  prism_ = prism;
  syllables_in_order_ = -1;  // 蓬萊：換了詞典要重查（SyllablesInOrder、TopLowerBound）
  syllable_count_ = 0;
  top_bounds_.clear();
}

bool UserDictionary::Load() {
  if (!db_ || db_->disabled())
    return false;
  if (!db_->loaded() && !db_->Open()) {
    // try to recover managed db in available work thread
    Deployer& deployer(Service::instance().deployer());
    auto task = DeploymentTask::Require("userdb_recovery_task");
    if (task && Is<Recoverable>(db_) && !deployer.IsWorking()) {
      deployer.ScheduleTask(an<DeploymentTask>(task->Create(db_)));
      deployer.StartWork();
    }
    return false;
  }
  return FetchTickCount() || Initialize();
}

bool UserDictionary::loaded() const {
  return db_ && !db_->disabled() && db_->loaded();
}

bool UserDictionary::readonly() const {
  return db_ && db_->readonly();
}

// this is a one-pass scan for the user db which supports sequential access
// in alphabetical order (of syllables).
// each call to DfsLookup() searches for matching phrases at a given
// start position: current_pos.
// there may be multiple edges that start at current_pos, and ends at different
// positions after current_pos. on each edge, there can be multiple syllables
// the spelling on the edge maps to.
// in order to enable forward scanning and to avoid backdating, our strategy is:
// sort all those syllables from edges that starts at current_pos, so that
// the syllables are in the same alphabetical order as the user db's.
// this having been done by transposing the syllable graph into
// SyllableGraph::index.
// however, in the case of 'shsh' which could be the abbreviation of either
// 'sh(a) sh(i)' or 'sh(a) s(hi) h(ou)',
// we now have to give up the latter path in order to avoid backdating.

// update: 2013-06-25
// to fix the following issue, we have to reintroduce backdating in db scan:
// given aaa=A, b=B, ab=C, derive/^(aa)a$/$1/,
// the input 'aaab' can be either aaa'b=AB or aa'ab=AC.
// note that backdating works only for normal or fuzzy spellings, but not for
// abbreviations such as 'shsh' in the previous example.

void UserDictionary::DfsLookup(const SyllableGraph& syll_graph,
                               size_t current_pos,
                               const string& current_prefix,
                               DfsState* state) {
  if (current_pos > state->watermark)
    state->watermark = current_pos;
  auto index = syll_graph.indices.find(current_pos);
  if (index == syll_graph.indices.end()) {
    return;
  }
  DLOG(INFO) << "dfs lookup starts from " << current_pos;
  // 蓬萊：簡拼的頂點一次 40–90 個音節，接在前綴後面的學習紀錄通常只有幾筆、對得上的不到 2%。
  // 有記憶體副本時，先找出前綴在陣列裡的範圍 [lo, hi)（最上層是整個陣列；下一層從上一層停的那一筆往兩邊找），
  // 每個音節找範圍裡第一筆 ≥「前綴＋音節＋空白」的：跟 ForwardScan 的 Jump 落在同一筆（整個陣列的
  // lower_bound 一定在範圍裡）。音節照拼法順序來，從上一個音節找到的地方往後找（不照順序時從範圍開頭找）。
  // 範圍裡沒有以它開頭的音節不必 Jump、讀那一筆：原本那樣做只會更新水位、把讀取位置留在 lower_bound，
  // 不會收詞也不會往下走。這裡照樣更新水位，讀取位置先記著（pending），要用之前（下一個要處理的音節、
  // 回傳給上一層之前）才設成原本會停的那一筆；下面原本的「前綴對不上就提早結束」也照那一筆判斷。
  // 收到的詞、順序、水位、讀取位置都跟一個一個 Jump 相同
  const auto* rows = state->rows;
  const bool filter = rows && !state->ranges.empty();
  size_t lo = 0, hi = 0;
  if (filter) {
    const auto& outer = state->ranges.back();
    if (current_prefix.empty()) {
      lo = outer.first;
      hi = outer.second;
    } else {
      const size_t at = state->accessor->RecordIndex();
      if (at > 0 && at <= rows->size() &&
          boost::starts_with((*rows)[at - 1].first, current_prefix)) {
        // 上一層剛讀到的那一筆以前綴開頭：範圍從那裡往兩邊找
        lo = at - 1;
        while (lo > outer.first &&
               boost::starts_with((*rows)[lo - 1].first, current_prefix))
          --lo;
        hi = at;
        size_t step = 1;
        while (hi < outer.second &&
               boost::starts_with((*rows)[hi].first, current_prefix)) {
          // 往後指數搜尋
          size_t probe = (std::min)(hi + step, outer.second) - 1;
          if (boost::starts_with((*rows)[probe].first, current_prefix)) {
            hi = probe + 1;
            step <<= 1;
          } else {
            hi = std::partition_point(
                     rows->begin() + hi, rows->begin() + probe,
                     [&](const pair<string, string>& row) {
                       return boost::starts_with(row.first, current_prefix);
                     }) -
                 rows->begin();
            break;
          }
        }
      } else {
        auto begin = std::lower_bound(
            rows->begin() + outer.first, rows->begin() + outer.second,
            current_prefix,
            [](const pair<string, string>& row, const string& k) {
              return row.first < k;
            });
        auto end = std::partition_point(
            begin, rows->begin() + outer.second,
            [&](const pair<string, string>& row) {
              return boost::starts_with(row.first, current_prefix);
            });
        lo = begin - rows->begin();
        hi = end - rows->begin();
      }
    }
    state->ranges.emplace_back(lo, hi);
  }
  BOOST_SCOPE_EXIT((&state)(filter)) {
    if (filter)
      state->ranges.pop_back();
  }
  BOOST_SCOPE_EXIT_END
  bool pending = false;
  size_t pending_index = 0;
  auto settle = [&]() {
    if (pending) {
      state->PlaceAfter(pending_index);
      pending = false;
    }
  };
  const size_t base = current_prefix.size();
  const bool in_order = filter && SyllablesInOrder();
  const SpellingIndex& spellings = index->second;
  const size_t num_syllables = spellings.size();
  // 一次跳過一串沒有的音節（下面）：音節編號都在 SyllablesInOrder 查過的範圍裡（拼法都不是空的）才做
  const bool can_skip = in_order && num_syllables > 0 &&
                        spellings.begin()->first >= 0 &&
                        static_cast<size_t>(spellings.rbegin()->first) <
                            syllable_count_;
  size_t cursor = lo;               // 上一個音節的 lower_bound
  const string* last_syllable = nullptr;
  string prefix;
  size_t present_index = 0;  // filter 時，學習紀錄裡有的這個音節的落點
  // 蓬萊：最上層（前綴是空的、範圍是整個副本）每個音節的落點就是整個陣列的 lower_bound，照音節編號記著
  //（TopLowerBound），不用每個都往後指數搜尋。音節照拼法順序來，上一個的落點不會在它後面，結果跟 gallop_after 相同
  const bool top_cache = can_skip && current_prefix.empty() &&
                         state->has_version && lo == 0 && hi > 0 &&
                         hi == rows->size() && hi < UINT32_MAX;
  for (size_t k = 0; k < num_syllables; ++k) {
    const auto& spelling = *(spellings.begin() + k);
    DLOG(INFO) << "prefix: '" << current_prefix << "'"
               << ", syll_id: " << spelling.first
               << ", num_spellings: " << spelling.second.size();
    if (filter) {
      const string& syllable = SyllableSpelling(spelling.first);
      if (syllable.empty()) {  // AppendSyllableToPrefix 失敗：原本直接換下一個音節
        LOG(ERROR) << "Error translating syllable_id '" << spelling.first
                   << "'.";
        continue;
      }
      // 音節編號的順序就是拼法的順序時（SyllablesInOrder）不用每個都比
      if (!in_order && last_syllable &&
          syllable_less(syllable, *last_syllable))
        cursor = lo;
      last_syllable = &syllable;
      int cmp = 1;
      if (top_cache)
        cursor = TopLowerBound(*rows, state->data_version, spelling.first,
                               syllable, &cmp);
      else
        cursor = gallop_after(*rows, cursor, hi, base, syllable, &cmp);
      const size_t found = cursor;
      if (found < hi && cmp == 0) {
        settle();  // 學習紀錄裡有：照原本的做法處理這個音節
        present_index = found;
      } else {
        if (!spelling.second.empty()) {  // 沒有邊時原本不 Jump，讀取位置不動
          for (size_t i = 0; i < spelling.second.size(); ++i) {
            auto props = spelling.second[i];
            if (i > 0 && props->type >= kAbbreviation)
              continue;
            if (props->end_pos > state->watermark)
              state->watermark = props->end_pos;
          }
          pending = true;
          pending_index = found;
        }
        // 原本的提早結束：讀取位置那一筆不是以前綴開頭（前綴是空的時候一定對得上，讀到底的空字串也算）
        const bool in_range =
            current_prefix.empty() ||
            (pending ? pending_index < hi
                     : boost::starts_with(*state->key, current_prefix));
        if (!in_range) {
          settle();
          return;
        }
        // 蓬萊：落點那一筆接在前綴後面的音節 T 比這個音節大（cmp > 0）：照順序接下來比 T 小的音節，
        // 落點也都是這一筆（前面的都比較小），一樣沒有、一樣只更新水位，讀取位置還是記著這一筆。
        // 一次跳到第一個不比 T 小的音節，水位照 dfs_reach 補上（前面的音節都處理過了，前綴最大值就是對的）
        if (can_skip && found < hi && cmp > 0 && k + 1 < num_syllables) {
          const string& row = (*rows)[found].first;
          size_t t_end = base;
          while (t_end < row.size() &&
                 static_cast<unsigned char>(row[t_end]) > ' ')
            ++t_end;
          if (t_end > base && t_end < row.size() && row[t_end] == ' ') {
            const char* t = row.data() + base;
            const size_t tn = t_end - base;
            auto less_t = [&](size_t j) {
              const string& s = SyllableSpelling((spellings.begin() + j)->first);
              return syllable_less(s.data(), s.size(), t, tn);
            };
            // [k + 1, num_syllables) 裡第一個不比 T 小的，從 k + 1 往後指數搜尋
            size_t next = k + 1;
            if (less_t(next)) {
              size_t lo2 = next + 1;  // lo2 之前的都比 T 小
              size_t step = 1;
              size_t end2 = num_syllables;
              while (true) {
                size_t probe = lo2 + step - 1;
                if (probe >= num_syllables)
                  break;
                if (!less_t(probe)) {
                  end2 = probe;
                  break;
                }
                lo2 = probe + 1;
                step <<= 1;
              }
              // 答案在 [lo2, end2] 之間
              while (lo2 < end2) {
                size_t mid = lo2 + (end2 - lo2) / 2;
                if (less_t(mid))
                  lo2 = mid + 1;
                else
                  end2 = mid;
              }
              next = lo2;
              const size_t reach = spellings.dfs_reach(next - 1);
              if (reach > state->watermark)
                state->watermark = reach;
              k = next - 1;  // 迴圈接著 ++k
            }
          }
        }
        continue;
      }
    }
    state->code.push_back(spelling.first);
    BOOST_SCOPE_EXIT((&state)) {
      state->code.pop_back();
    }
    BOOST_SCOPE_EXIT_END
    // 蓬萊：等於 TranslateCodeToString(state->code, &prefix)，只是從上一層的前綴往後接
    if (!AppendSyllableToPrefix(spelling.first, current_prefix, &prefix))
      continue;
    for (size_t i = 0; i < spelling.second.size(); ++i) {
      auto props = spelling.second[i];
      if (i > 0 && props->type >= kAbbreviation)
        continue;
      state->credibility.push_back(state->credibility.back() +
                                   props->credibility);
      size_t end_pos = props->end_pos;
      if (end_pos > state->watermark)
        state->watermark = end_pos;
      // 全碼匹配長度積分
      bool is_normal_spelling = props->type == kNormalSpelling;
      double delta_quality_len =
          (is_normal_spelling ? 1.0 : 0.0) * (end_pos - current_pos);
      state->quality_len.push_back(state->quality_len.back() +
                                   delta_quality_len);
      BOOST_SCOPE_EXIT((&state)) {
        state->credibility.pop_back();
        state->quality_len.pop_back();
      }
      BOOST_SCOPE_EXIT_END
      DLOG(INFO) << "edge: [" << current_pos << ", " << end_pos << ")";
      if (prefix != *state->key) {  // 'a b c |d ' > 'a b c \tabracadabra'
        DLOG(INFO) << "forward scanning for '" << prefix << "'.";
        if (filter) {
          // 蓬萊：整個陣列裡第一筆 ≥ prefix 的就是落點 present_index（前面的都比較小），
          // 跟 Jump(prefix) 再讀一筆的結果相同，不用再二分搜尋
          state->PlaceAfter(present_index);
        } else if (!state->ForwardScan(prefix)) {  // reached the end of db
          continue;
        }
      }
      while (state->IsExactMatch(prefix)) {  // 'b |e ' vs. 'b e \tBe'
        DLOG(INFO) << "match found for '" << prefix << "'.";
        state->RecruitEntry(end_pos);
        if (!state->NextEntry())  // reached the end of db
          break;
      }
      auto next_index = syll_graph.indices.find(end_pos);
      if (next_index == syll_graph.indices.end()) {
        // reached the end of input, predict word if requested
        if (state->predict_word_from_depth != 0 &&
            state->depth() >= state->predict_word_from_depth) {
          while (state->IsPrefixMatch(prefix)) {
            DLOG(INFO) << "prefix match found for '" << prefix << "'.";
            if (syllabary_.empty()) {
              Syllabary syllabary;
              if (!table_->GetSyllabary(&syllabary)) {
                LOG(ERROR) << "failed to get syllabary for user dict: "
                           << name();
                break;
              }
              SyllableId syllable_id = 0;
              for (auto s = syllabary.begin(); s != syllabary.end(); ++s) {
                syllabary_[*s] = syllable_id++;
              }
            }
            state->RecruitEntry(end_pos, &syllabary_);
            if (!state->NextEntry())  // reached the end of db
              break;
          }
        }
      } else {
        // the caller can limit the number of syllables to look up
        if ((!state->depth_limit || state->depth() < state->depth_limit) &&
            state->IsPrefixMatch(prefix)) {  // 'b |e ' vs. 'b e f \tBefore'
          DfsLookup(syll_graph, end_pos, prefix, state);
        }
      }
    }
    if (!state->IsPrefixMatch(current_prefix))  // 'b |' vs. 'g o \tGo'
      return;
    // 'b |e ' vs. 'b y \tBy'
  }
  settle();
}

static an<UserDictEntryCollector> collect(
    hash_map<int, DictEntryList>* source) {
  auto result = New<UserDictEntryCollector>();
  for (auto& x : *source) {
    (*result)[x.first].SetEntries(std::move(x.second));
  }
  return result;
}

an<UserDictEntryCollector> UserDictionary::Lookup(
    const SyllableGraph& syll_graph,
    size_t start_pos,
    size_t depth_limit,
    size_t predict_word_from_depth,
    double initial_credibility,
    size_t* watermark) {
  if (watermark && start_pos > *watermark)
    *watermark = start_pos;
  if (!table_ || !prism_ || !loaded() ||
      start_pos >= syll_graph.interpreted_length)
    return nullptr;
  DfsState state;
  state.watermark = start_pos;
  state.depth_limit = depth_limit;
  state.predict_word_from_depth = predict_word_from_depth;
  FetchTickCount();
  state.present_tick = tick_ + 1;
  state.credibility.push_back(initial_credibility);
  state.quality_len.push_back(0.0);
  // 蓬萊：LevelDB 的學習紀錄改查記憶體副本（level_db.cc 的 LevelDbMirror），順序和內容跟 Query("") 相同
  if (auto level_db = As<LevelDb>(db_)) {
    state.accessor = level_db->QueryUserEntries();
    state.has_version = true;
    state.data_version = level_db->version();
  } else {
    state.accessor = db_->Query("");
  }
  state.accessor->Jump(" ");  // skip metadata
  // 蓬萊：記憶體副本只放 key ≥ " " 的詞條，最上層的範圍就是整個陣列
  state.rows = state.accessor->SortedRecords();
  if (state.rows)
    state.ranges.emplace_back(0, state.rows->size());
  string prefix;
  DfsLookup(syll_graph, start_pos, prefix, &state);
  if (watermark && state.watermark > *watermark)
    *watermark = state.watermark;
  if (state.query_result.empty())
    return nullptr;
  // sort each group of homophones by weight
  for (auto& v : state.query_result) {
    auto& entries = v.second;
    entries.Sort();
    if (state.predict_word_from_depth) {
      if (!entries.empty() && entries.front()->IsPredictiveMatch()) {
        DLOG(INFO) << "front entry is predictive match: "
                   << entries.front()->text;
        auto found =
            std::find_if(entries.begin(), entries.end(),
                         [](const auto& e) { return e->IsExactMatch(); });
        if (found != entries.end()) {
          DLOG(INFO) << "rotating exact match entry to front: "
                     << (*found)->text;
          std::rotate(entries.begin(), found, found + 1);
        }
      }
    }
  }
  return collect(&state.query_result);
}

size_t UserDictionary::LookupWords(UserDictEntryIterator* result,
                                   const string& input,
                                   bool predictive,
                                   size_t limit,
                                   string* resume_key) {
  TickCount present_tick = tick_ + 1;
  size_t len = input.length();
  size_t start = result->cache_size();
  size_t count = 0;
  size_t exact_match_count = 0;
  const string kEnd = "\xff";
  string key;
  string value;
  string full_code;
  auto accessor = db_->Query(input);
  if (!accessor || accessor->exhausted()) {
    if (resume_key)
      *resume_key = kEnd;
    return 0;
  }
  if (resume_key && !resume_key->empty()) {
    if (!accessor->Jump(*resume_key) ||
        !accessor->GetNextRecord(&key, &value)) {
      *resume_key = kEnd;
      return 0;
    }
    DLOG(INFO) << "resume lookup after: " << key;
  }
  string last_key(key);
  while (accessor->GetNextRecord(&key, &value)) {
    DLOG(INFO) << "key : " << key << ", value: " << value;
    bool is_exact_match = (len < key.length() && key[len] == ' ');
    if (!is_exact_match && !predictive) {
      key = last_key;
      break;
    }
    last_key = key;
    auto e = CreateDictEntry(key, value, present_tick, 1.0, len, &full_code);
    if (!e)
      continue;
    e->custom_code = full_code;
    boost::trim_right(full_code);  // remove trailing space a user dict key has
    if (full_code.length() > len) {
      e->comment = "~" + full_code.substr(len);
      e->remaining_code_length = full_code.length() - len;
    }
    result->Add(std::move(e));
    ++count;
    if (is_exact_match)
      ++exact_match_count;
    else if (limit && count >= limit)
      break;
  }
  if (exact_match_count > 0) {
    result->SortRange(start, exact_match_count);
  }
  if (resume_key) {
    *resume_key = key;
    DLOG(INFO) << "resume key reset to: " << *resume_key;
  }
  return count;
}

bool UserDictionary::UpdateEntry(const DictEntry& entry, int commits) {
  return UpdateEntry(entry, commits, "");
}

bool UserDictionary::UpdateEntry(const DictEntry& entry,
                                 int commits,
                                 const string& new_entry_prefix) {
  string code_str(entry.custom_code);
  if (code_str.empty() && !TranslateCodeToString(entry.code, &code_str))
    return false;
  string key(code_str + '\t' + entry.text);
  string value;
  UserDbValue v;
  if (db_->Fetch(key, &value)) {
    v.Unpack(value);
    if (v.tick > tick_) {
      v.tick = tick_;  // fix abnormal timestamp
    }
  } else if (!new_entry_prefix.empty()) {
    key.insert(0, new_entry_prefix);
  }
  if (commits > 0) {
    if (v.commits < 0)
      v.commits = -v.commits;  // revive a deleted item
    v.commits += commits;
    UpdateTickCount(1);
    v.dee = algo::formula_d(commits, (double)tick_, v.dee, (double)v.tick);
  } else if (commits == 0) {
    const double k = 0.1;
    v.dee = algo::formula_d(k, (double)tick_, v.dee, (double)v.tick);
  } else if (commits < 0) {  // mark as deleted
    v.commits = (std::min)(-1, -v.commits);
    v.dee = algo::formula_d(0.0, (double)tick_, v.dee, (double)v.tick);
  }
  v.tick = tick_;
  return db_->Update(key, v.Pack());
}

bool UserDictionary::FetchCommits(const DictEntry& entry, int* commits) {
  if (!loaded())
    return false;
  string code_str(entry.custom_code);
  if (code_str.empty() && !TranslateCodeToString(entry.code, &code_str))
    return false;
  string value;
  if (!db_->Fetch(code_str + '\t' + entry.text, &value))
    return false;
  UserDbValue v(value);
  if (v.commits < 0)
    return false;
  if (commits)
    *commits = v.commits;
  return true;
}

bool UserDictionary::UpdateTickCount(TickCount increment) {
  tick_ += increment;
  try {
    return db_->MetaUpdate("/tick", std::to_string(tick_));
  } catch (...) {
    return false;
  }
}

bool UserDictionary::Initialize() {
  return db_->MetaUpdate("/tick", "0");
}

bool UserDictionary::FetchTickCount() {
  string value;
  try {
    // an earlier version mistakenly wrote tick count into an empty key
    if (!db_->MetaFetch("/tick", &value) && !db_->Fetch("", &value))
      return false;
    tick_ = std::stoul(value);
    return true;
  } catch (...) {
    // tick_ = 0;
    return false;
  }
}

bool UserDictionary::NewTransaction() {
  auto db = As<Transactional>(db_);
  if (!db)
    return false;
  CommitPendingTransaction();
  transaction_time_ = time(NULL);
  return db->BeginTransaction();
}

bool UserDictionary::RevertRecentTransaction() {
  auto db = As<Transactional>(db_);
  if (!db || !db->in_transaction())
    return false;
  if (time(NULL) - transaction_time_ > scoring().revert_window)
    return false;
  return db->AbortTransaction();
}

bool UserDictionary::CommitPendingTransaction() {
  auto db = As<Transactional>(db_);
  if (db && db->in_transaction()) {
    return db->CommitTransaction();
  }
  return false;
}

size_t UserDictionary::data_version() const {
  if (auto level_db = As<LevelDb>(db_))
    return level_db->version();
  return LevelDb::NewVersion();
}

bool UserDictionary::SyllablesInOrder() {
  // 蓬萊：編號照詞典音節表的順序給（同 DfsLookup 預測詞時建的 syllabary_）；相鄰兩個都照 ForwardScan 的順序
  // （「拼法＋空白」）由小到大，DfsLookup 照編號走的音節就是照順序來，不用每次比前一個
  if (syllables_in_order_ < 0) {
    syllables_in_order_ = 0;
    Syllabary syllabary;
    if (table_ && table_->GetSyllabary(&syllabary)) {
      bool ok = true;
      const string* last = nullptr;
      SyllableId id = 0;
      for (const auto& s : syllabary) {
        // 編號真的照這個順序給的才算（GetSyllableById 跟音節表逐一相同）；
        // 拼法不是空的、沒有空白和控制字元（DfsLookup 一次跳過一串音節時，靠這個跟學習紀錄的 key 比）
        if (s.empty() ||
            std::any_of(s.begin(), s.end(),
                        [](char c) {
                          return static_cast<unsigned char>(c) <= ' ';
                        }) ||
            table_->GetSyllableById(id++) != s ||
            (last && !syllable_less(*last, s))) {
          ok = false;
          break;
        }
        last = &s;
      }
      syllables_in_order_ = ok ? 1 : 0;
      syllable_count_ = ok ? syllabary.size() : 0;
    }
  }
  return syllables_in_order_ == 1;
}

size_t UserDictionary::TopLowerBound(const vector<pair<string, string>>& rows,
                                     size_t version,
                                     SyllableId syllable_id,
                                     const string& spelling,
                                     int* cmp) {
  constexpr uint32_t kUnknown = UINT32_MAX;
  if (top_rows_ != &rows || top_rows_size_ != rows.size() ||
      top_version_ != version || top_bounds_.size() != syllable_count_) {
    top_rows_ = &rows;
    top_rows_size_ = rows.size();
    top_version_ = version;
    top_bounds_.assign(syllable_count_, TopBound{kUnknown, 0});
  }
  auto& b = top_bounds_[syllable_id];  // 呼叫的地方確認過編號 < syllable_count_
  if (b.index == kUnknown) {
    const size_t found =
        std::partition_point(rows.begin(), rows.end(),
                             [&](const pair<string, string>& row) {
                               return compare_after(row.first, 0, spelling) < 0;
                             }) -
        rows.begin();
    b.index = static_cast<uint32_t>(found);
    b.cmp = found < rows.size() ? compare_after(rows[found].first, 0, spelling)
                                : 1;
  }
  *cmp = b.cmp;
  return b.index;
}

const string& UserDictionary::SyllableSpelling(SyllableId syllable_id) {
  // 蓬萊：查過的放進陣列（指著 rev_syllabary_ 的值；boost::unordered_map 的元素位址不會變）
  if (syllable_id >= 0 &&
      static_cast<size_t>(syllable_id) < spelling_by_id_.size() &&
      spelling_by_id_[syllable_id])
    return *spelling_by_id_[syllable_id];
  auto found = rev_syllabary_.find(syllable_id);
  const string& spelling =
      found != rev_syllabary_.end()
          ? found->second
          : (rev_syllabary_[syllable_id] =
                 table_->GetSyllableById(syllable_id));
  if (syllable_id >= 0 && syllable_id < (1 << 20)) {
    if (static_cast<size_t>(syllable_id) >= spelling_by_id_.size())
      spelling_by_id_.resize(syllable_id + 1, nullptr);
    spelling_by_id_[syllable_id] = &spelling;
  }
  return spelling;
}

bool UserDictionary::AppendSyllableToPrefix(SyllableId syllable_id,
                                            const string& base,
                                            string* result) {
  if (!table_ || !result)
    return false;
  const string& spelling = SyllableSpelling(syllable_id);
  if (spelling.empty()) {
    LOG(ERROR) << "Error translating syllable_id '" << syllable_id << "'.";
    result->clear();
    return false;
  }
  result->assign(base);
  result->append(spelling);
  result->push_back(' ');
  return true;
}

bool UserDictionary::TranslateCodeToString(const Code& code, string* result) {
  if (!table_ || !result)
    return false;
  result->clear();
  for (const SyllableId& syllable_id : code) {
    string spelling = rev_syllabary_.count(syllable_id)
                          ? rev_syllabary_[syllable_id]
                          : (rev_syllabary_[syllable_id] =
                                 table_->GetSyllableById(syllable_id));
    if (spelling.empty()) {
      LOG(ERROR) << "Error translating syllable_id '" << syllable_id << "'.";
      result->clear();
      return false;
    }
    *result += spelling;
    *result += ' ';
  }
  return true;
}

an<DictEntry> UserDictionary::CreateDictEntry(const string& key,
                                              const string& value,
                                              TickCount present_tick,
                                              double credibility,
                                              double quality_len,
                                              string* full_code) {
  an<DictEntry> e;
  size_t separator_pos = key.find('\t');
  if (separator_pos == string::npos)
    return e;
  UserDbValue v;
  if (!v.Unpack(value))
    return e;
  if (v.commits < 0)  // deleted entry
    return e;
  if (v.tick < present_tick)
    v.dee = algo::formula_d(0, (double)present_tick, v.dee, (double)v.tick);
  // create!
  e = New<DictEntry>();
  e->text = key.substr(separator_pos + 1);
  e->commit_count = v.commits;
  const auto& sc = scoring();
  double weight;
  if (sc.mode == 1) {
    // 頻率為主、新近為輔：一次剛打的新近值（dee≈1）只折合 recency 次打字，
    // 原版公式裡一次新近大約等於 200 次舊的打字。
    double prior = 0.0;
    if (sc.prior_weight > 0 && !sc.prior.empty()) {
      auto found = sc.prior.find(e->text);
      if (found != sc.prior.end())
        prior = found->second;
    }
    weight = sc.unit * (v.commits + sc.recency * v.dee + sc.prior_weight * prior);
  } else {
    // TODO: argument s not defined...
    weight = algo::formula_p(0, (double)v.commits / present_tick,
                             (double)present_tick, v.dee);
  }
  e->weight = log(weight > 0 ? weight : DBL_EPSILON) + credibility;
  e->quality_len = quality_len;
  if (full_code) {
    *full_code = key.substr(0, separator_pos);
  }
  DLOG(INFO) << "text = '" << e->text << "', code_len = " << e->code.size()
             << ", weight = " << e->weight
             << ", quality_len = " << e->quality_len
             << ", commit_count = " << e->commit_count
             << ", present_tick = " << present_tick;
  return e;
}

// UserDictionaryComponent members

UserDictionaryComponent::UserDictionaryComponent() {}

UserDictionary* UserDictionaryComponent::Create(const string& dict_name,
                                                const string& db_class) {
  auto db = db_pool_[dict_name].lock();
  if (!db) {
    auto component = Db::Require(db_class);
    if (!component) {
      LOG(ERROR) << "undefined db class '" << db_class << "'.";
      return NULL;
    }
    db.reset(component->Create(dict_name));
    db_pool_[dict_name] = db;
  }
  return new UserDictionary(dict_name, db);
}

UserDictionary* UserDictionaryComponent::Create(const Ticket& ticket) {
  if (!ticket.schema)
    return NULL;
  Config* config = ticket.schema->config();
  bool enable_user_dict = true;
  config->GetBool(ticket.name_space + "/enable_user_dict", &enable_user_dict);
  if (!enable_user_dict)
    return NULL;
  string dict_name;
  if (config->GetString(ticket.name_space + "/user_dict", &dict_name)) {
    // user specified name
  } else if (config->GetString(ticket.name_space + "/dictionary", &dict_name)) {
    // {dictionary: luna_pinyin.extra} implies {user_dict: luna_pinyin}
    dict_name = Language::get_language_component(dict_name);
  } else {
    LOG(ERROR) << ticket.name_space << "/dictionary not specified in schema '"
               << ticket.schema->schema_id() << "'.";
    return NULL;
  }
  string db_class("userdb");
  if (config->GetString(ticket.name_space + "/db_class", &db_class)) {
    // user specified db class
  }
  // obtain userdb object
  return Create(dict_name, db_class);
}

}  // namespace rime
