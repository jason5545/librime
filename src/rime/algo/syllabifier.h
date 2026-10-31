//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-7-12 Zou xu <zouivex@gmail.com>
//

#ifndef RIME_SYLLABIFIER_H_
#define RIME_SYLLABIFIER_H_

#include <stdint.h>
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <rime_api.h>
#include <rime/common.h>
#include "spelling.h"

namespace rime {

class Prism;
class Corrector;

using SyllableId = int32_t;

struct EdgeProperties : SpellingProperties {
  EdgeProperties(SpellingProperties sup) : SpellingProperties(sup) {};
  EdgeProperties() = default;
  // 切分歧義編碼段的起始位置
  set<size_t> ambiguous_source_positions;
};

// 蓬萊：一條邊（起點、終點）上的音節原本是 std::map，簡拼的邊一次 40–90 個，每鍵整張圖重建一次，
// 每個音節配置、釋放一個節點。改成 flat_map（照音節編號排好的連續陣列，find、走訪、insert、operator[] 一樣）；
// 建好之後不再增刪（Transpose 指著裡面的 EdgeProperties）
// 蓬萊：flat_map 每條邊一塊連續記憶體，大小是 88 位元組乘上音節數（簡拼的邊 40–90 個），每鍵整張圖重建，
// 配置器照大小分級，各種大小散在幾十個級距，每個級距都留著用過的頁（std::map 的節點全是同一個大小）：
// 引擎記憶體多 0.5 MB 上下。配置的位元組數進位到 2 的次方（256 位元組以下照原本），級距只剩幾個，釋放的塊下一鍵就用得上。
// 只改向系統要的大小，容器的容量、元素、順序都不變
template <class T>
struct RoundedAllocator {
  using value_type = T;
  RoundedAllocator() noexcept = default;
  template <class U>
  RoundedAllocator(const RoundedAllocator<U>&) noexcept {}
  static size_t Rounded(size_t bytes) {
    if (bytes <= 256)
      return bytes;
    size_t r = 512;
    while (r < bytes)
      r <<= 1;
    return r;
  }
  T* allocate(size_t n) {
    return static_cast<T*>(::operator new(Rounded(n * sizeof(T))));
  }
  void deallocate(T* p, size_t) noexcept { ::operator delete(p); }
  template <class U>
  bool operator==(const RoundedAllocator<U>&) const noexcept { return true; }
  template <class U>
  bool operator!=(const RoundedAllocator<U>&) const noexcept { return false; }
};

using SpellingMap =
    boost::container::flat_map<SyllableId,
                               EdgeProperties,
                               std::less<SyllableId>,
                               RoundedAllocator<std::pair<SyllableId, EdgeProperties>>>;
using VertexMap = map<size_t, SpellingType>;
using EndVertexMap = map<size_t, SpellingMap>;
using EdgeMap = map<size_t, EndVertexMap>;

// 蓬萊：一個音節的邊（指著 SpellingIndex 裡連續的一段，跟原本的 vector 一樣可以 size()、[i]、range-for）
class SpellingPropertiesList {
 public:
  using const_iterator = const EdgeProperties* const*;
  SpellingPropertiesList() = default;
  SpellingPropertiesList(const_iterator b, const_iterator e)
      : begin_(b), end_(e) {}
  const_iterator begin() const { return begin_; }
  const_iterator end() const { return end_; }
  size_t size() const { return end_ - begin_; }
  bool empty() const { return begin_ == end_; }
  const EdgeProperties* operator[](size_t i) const { return begin_[i]; }

 private:
  const_iterator begin_ = nullptr;
  const_iterator end_ = nullptr;
};

// 蓬萊：一個頂點出去的「音節 → 邊」，原本是 map<SyllableId, vector<const EdgeProperties*>>。
// 改成兩個連續的陣列（音節照編號由小到大、每個音節的邊照原本 push_back 的順序），走訪的順序、內容都跟 map 相同；
// Transpose 每個頂點配置兩次，不是每個音節一個 map 節點加一個 vector，查詢走訪、find 也不用跟著指標跳
class SpellingIndex {
 public:
  struct value_type {
    SyllableId first;
    SpellingPropertiesList second;
    // 到這個音節為止（第 0 到這一個）照 UserDictionary::DfsLookup 更新水位的規則（每個音節第一條邊、
    // 其他不是簡拼的邊）最遠到哪：DfsLookup 一次跳過一串學習紀錄裡沒有的音節時用
    size_t dfs_reach;
  };
  using const_iterator = vector<value_type>::const_iterator;
  using const_reverse_iterator = vector<value_type>::const_reverse_iterator;

  SpellingIndex() = default;
  SpellingIndex(const SpellingIndex& other) { *this = other; }
  SpellingIndex& operator=(const SpellingIndex& other) {
    if (this != &other) {
      items_ = other.items_;
      props_ = other.props_;
      Rebase();
    }
    return *this;
  }
  // vector 搬走時緩衝區不變，指標照樣有效
  SpellingIndex(SpellingIndex&&) = default;
  SpellingIndex& operator=(SpellingIndex&&) = default;

  const_iterator begin() const { return items_.begin(); }
  const_iterator end() const { return items_.end(); }
  const_reverse_iterator rbegin() const { return items_.rbegin(); }
  const_reverse_iterator rend() const { return items_.rend(); }
  size_t size() const { return items_.size(); }
  bool empty() const { return items_.empty(); }
  const_iterator find(SyllableId syllable_id) const {
    auto it = std::lower_bound(
        items_.begin(), items_.end(), syllable_id,
        [](const value_type& x, SyllableId id) { return x.first < id; });
    return it != items_.end() && it->first == syllable_id ? it : items_.end();
  }
  size_t dfs_reach(size_t k) const { return items_[k].dfs_reach; }
  // 沒有這個音節時是空的
  SpellingPropertiesList operator[](SyllableId syllable_id) const {
    auto it = find(syllable_id);
    return it != items_.end() ? it->second : SpellingPropertiesList();
  }

  // 重建：先 Reset(邊的總數)，再照音節編號由小到大 Append（同一個音節照原本 push_back 的順序）。
  // props_ 預留了總數，Append 不會搬家，指著它的範圍一直有效
  void Reset(size_t total) {
    items_.clear();
    props_.clear();
    items_.reserve(total);
    props_.reserve(total);
  }
  void Append(SyllableId syllable_id, const EdgeProperties* props) {
    props_.push_back(props);
    const EdgeProperties* const* last = props_.data() + props_.size();
    if (items_.empty() || items_.back().first != syllable_id) {
      const size_t reach =
          items_.empty() ? props->end_pos
                         : (std::max)(items_.back().dfs_reach, props->end_pos);
      items_.push_back(
          {syllable_id, SpellingPropertiesList(last - 1, last), reach});
    } else {
      auto& item = items_.back();
      item.second = SpellingPropertiesList(item.second.begin(), last);
      if (props->type < kAbbreviation && props->end_pos > item.dfs_reach)
        item.dfs_reach = props->end_pos;
    }
  }
  // 原本的 index[syllable_id].push_back(props)：接在那個音節的最後面
  void Add(SyllableId syllable_id, const EdgeProperties* props) {
    vector<pair<SyllableId, const EdgeProperties*>> all;
    bool added = false;
    for (const auto& item : items_) {
      if (!added && syllable_id < item.first) {
        all.emplace_back(syllable_id, props);
        added = true;
      }
      for (auto p : item.second)
        all.emplace_back(item.first, p);
      if (!added && syllable_id == item.first) {
        all.emplace_back(syllable_id, props);
        added = true;
      }
    }
    if (!added)
      all.emplace_back(syllable_id, props);
    Reset(all.size());
    for (const auto& x : all)
      Append(x.first, x.second);
  }

 private:
  // 複製之後（items_ 還指著來源的 props_）：照每個音節的個數重新指到自己的 props_
  void Rebase() {
    const EdgeProperties* const* p = props_.data();
    for (auto& item : items_) {
      const size_t n = item.second.size();
      item.second = SpellingPropertiesList(p, p + n);
      p += n;
    }
  }

  vector<value_type> items_;
  vector<const EdgeProperties*> props_;
};

using SpellingIndices = map<size_t, SpellingIndex>;

struct SyllableGraph {
  size_t input_length = 0;
  size_t interpreted_length = 0;
  VertexMap vertices;
  EdgeMap edges;
  SpellingIndices indices;
};

class Syllabifier {
 public:
  Syllabifier() = default;
  explicit Syllabifier(const string& delimiters,
                       bool enable_completion = false,
                       bool strict_spelling = false)
      : delimiters_(delimiters),
        enable_completion_(enable_completion),
        strict_spelling_(strict_spelling) {}

  RIME_DLL int BuildSyllableGraph(const string& input,
                                  Prism& prism,
                                  SyllableGraph* graph);
  RIME_DLL void EnableCorrection(Corrector* corrector);
  // 蓬萊：罕用黏合音節（docs/optimization.md 第 4 項第 4 條 4C）。這幾個拼法（prism 的拼法編號）底下的音節
  // 一律改成簡拼類型、多扣一次簡拼的分，跟方案檔 speller/algebra 寫 abbrev → erase → xform 三條規則
  // 部署出來的 prism 逐位元組相同的結果，只是改成打字時決定（照打法開關）。nullptr＝不改
  void SetGlueSpellings(const hash_set<SyllableId>* spellings) {
    glue_spellings_ = spellings;
  }

 protected:
  bool Glue(SyllableId spelling_id, SpellingProperties* props) const;
  void CheckOverlappedSpellings(SyllableGraph* graph, size_t start, size_t end);
  void Transpose(SyllableGraph* graph);

  string delimiters_;
  bool enable_completion_ = false;
  bool strict_spelling_ = false;
  Corrector* corrector_ = nullptr;
  const hash_set<SyllableId>* glue_spellings_ = nullptr;
};

}  // namespace rime

#endif  // RIME_SYLLABIFIER_H_
